#include "doctest.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "fdm/DownloadEngine.h"

namespace fs = std::filesystem;

// Reference values for the test payload "hi" (computed once with
// Python's hashlib + base64; see test_engine_digest.cpp commit log):
//   sha-256 hex = 8f434346648f6b96df89dda901c5176b10a6d83961dd3c1ac88b59b2dc327aa4
//   sha-256 b64 = j0NDRmSPa5bfid2pAcUXaxCm2Dlh3TwayItZstwyeqQ=
//   md5     hex = 49f68a5c8493ec2c0bf489821c21fc3b
//   md5     b64 = SfaKXIST7CwL9ImCHCH8Ow==
//
// URL-encoded base64 forms (= -> %3D, no + or / in these particular hashes):
constexpr const char* kSha256Hex = "8f434346648f6b96df89dda901c5176b10a6d83961dd3c1ac88b59b2dc327aa4";
constexpr const char* kSha256B64Enc = "j0NDRmSPa5bfid2pAcUXaxCm2Dlh3TwayItZstwyeqQ%3D";
constexpr const char* kMd5Hex = "49f68a5c8493ec2c0bf489821c21fc3b";
constexpr const char* kMd5B64Enc = "SfaKXIST7CwL9ImCHCH8Ow%3D%3D";

namespace {

int findFreePort() {
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t len = sizeof(a);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    const int port = ntohs(a.sin_port);
    ::close(s);
    return port;
}

class HttpServer {
public:
    HttpServer(const fs::path& root, int port) : port_(port) {
        const std::string script = std::string(FDM_TEST_SOURCE_DIR) + "/range_server.py";
        const std::string portStr = std::to_string(port_);
        const std::string rootStr = root.string();
        pid_ = ::fork();
        if (pid_ == 0) {
            const int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDERR_FILENO);
                ::dup2(devnull, STDOUT_FILENO);
                ::close(devnull);
            }
            ::execlp("python3", "python3", script.c_str(), portStr.c_str(),
                     rootStr.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        waitForPort();
    }
    ~HttpServer() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
    }
    int port() const { return port_; }

private:
    void waitForPort() {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            const int s = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = htons(port_);
            const int rc = ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
            ::close(s);
            if (rc == 0) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    pid_t pid_ = -1;
    int port_;
};

fdm::ProbeResult probeSync(fdm::DownloadEngine& engine, const std::string& url) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    fdm::ProbeResult result;
    engine.probe(url, [&](fdm::ProbeResult r) {
        std::lock_guard<std::mutex> lk(m);
        result = std::move(r);
        done = true;
        cv.notify_one();
    });
    std::unique_lock<std::mutex> lk(m);
    cv.wait_for(lk, std::chrono::seconds(5), [&] { return done; });
    return result;
}

struct DigestFixture {
    fs::path tmp;
    HttpServer server;

    DigestFixture()
        : tmp(fs::temp_directory_path() / "fdm_digest_test"),
          server((fs::create_directories(tmp),
                  std::ofstream(tmp / "payload.bin", std::ios::binary).write("hi", 2),
                  tmp),
                 findFreePort()) {}

    std::string url(const std::string& query) const {
        return "http://127.0.0.1:" + std::to_string(server.port()) +
               "/payload.bin?" + query;
    }
};

}  // namespace

TEST_CASE("probe extracts hash from Content-Digest sha-256 (RFC 9530)") {
    DigestFixture f;
    fdm::DownloadEngine engine;
    // Content-Digest: sha-256=:<base64>:
    const std::string q =
        std::string("cdigest=sha-256%3D%3A") + kSha256B64Enc + "%3A";
    const auto r = probeSync(engine, f.url(q));
    REQUIRE(r.ok);
    CHECK(r.info.hashAlgorithm == "sha256");
    CHECK(r.info.hashSource == "content-digest");
    CHECK(r.info.expectedHash == kSha256Hex);
}

TEST_CASE("probe extracts hash from RFC 3230 Digest header") {
    DigestFixture f;
    fdm::DownloadEngine engine;
    // Digest: sha-256=<base64>
    const std::string q = std::string("digest=sha-256%3D") + kSha256B64Enc;
    const auto r = probeSync(engine, f.url(q));
    REQUIRE(r.ok);
    CHECK(r.info.hashAlgorithm == "sha256");
    CHECK(r.info.hashSource == "digest");
    CHECK(r.info.expectedHash == kSha256Hex);
}

TEST_CASE("probe picks SHA-256 over MD5 when both listed in Digest") {
    DigestFixture f;
    fdm::DownloadEngine engine;
    // Digest: md5=<md5b64>, sha-256=<sha256b64>
    const std::string q = std::string("digest=md5%3D") + kMd5B64Enc +
                          "%2Csha-256%3D" + kSha256B64Enc;
    const auto r = probeSync(engine, f.url(q));
    REQUIRE(r.ok);
    CHECK(r.info.hashAlgorithm == "sha256");
    CHECK(r.info.expectedHash == kSha256Hex);
}

TEST_CASE("probe extracts hash from legacy Content-MD5 header") {
    DigestFixture f;
    fdm::DownloadEngine engine;
    // Content-MD5: <base64>
    const auto r = probeSync(engine, f.url(std::string("cmd5=") + kMd5B64Enc));
    REQUIRE(r.ok);
    CHECK(r.info.hashAlgorithm == "md5");
    CHECK(r.info.hashSource == "content-md5");
    CHECK(r.info.expectedHash == kMd5Hex);
}

TEST_CASE("probe returns empty expectedHash when no digest header present") {
    DigestFixture f;
    fdm::DownloadEngine engine;
    const std::string url =
        "http://127.0.0.1:" + std::to_string(f.server.port()) + "/payload.bin";
    const auto r = probeSync(engine, url);
    REQUIRE(r.ok);
    CHECK(r.info.expectedHash.empty());
    CHECK(r.info.hashAlgorithm.empty());
    CHECK(r.info.hashSource.empty());
}

TEST_CASE("probe rejects digest with wrong byte length for declared algorithm") {
    DigestFixture f;
    fdm::DownloadEngine engine;
    // sha-256 declared, but only 3 bytes of base64 -- decode fails / length mismatch.
    const auto r = probeSync(engine, f.url("cdigest=sha-256%3D%3AYWJj%3A"));
    REQUIRE(r.ok);
    CHECK(r.info.expectedHash.empty());
}

TEST_CASE("engine downloads a response that omits Content-Length") {
    // Server-side simulation: ?nolen=1 makes range_server.py respond with no
    // Content-Length, no Accept-Ranges, Connection: close. Mirrors real
    // CGI scripts / streaming endpoints that don't know the body length up
    // front. The engine should fall back to a single open-ended chunk and
    // still deliver the bytes.
    DigestFixture f;
    const fs::path outputPath = f.tmp / "output.bin";
    std::error_code ec;
    fs::remove(outputPath, ec);

    fdm::DownloadEngine engine;
    std::mutex m;
    std::condition_variable cv;
    bool terminal = false;
    bool ok = false;
    std::string error;
    std::int64_t startedContentLength = 99;  // sentinel != -1
    bool startedSupportsRanges = true;
    int chunkCount = -1;
    std::int64_t finalReceived = -1;

    engine.start(f.url("nolen=1"), outputPath.string(), [&](fdm::EngineEvent ev) {
        std::visit(
            [&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, fdm::Started>) {
                    startedContentLength = e.contentLength;
                    startedSupportsRanges = e.supportsRanges;
                    chunkCount = e.chunkCount;
                } else if constexpr (std::is_same_v<T, fdm::Progress>) {
                    finalReceived = e.received;  // overwritten by each progress tick
                } else if constexpr (std::is_same_v<T, fdm::Finished>) {
                    std::lock_guard<std::mutex> lk(m);
                    ok = true;
                    terminal = true;
                    cv.notify_one();
                } else if constexpr (std::is_same_v<T, fdm::Failed>) {
                    std::lock_guard<std::mutex> lk(m);
                    error = e.reason;
                    terminal = true;
                    cv.notify_one();
                }
            },
            ev);
    });
    {
        std::unique_lock<std::mutex> lk(m);
        REQUIRE(cv.wait_for(lk, std::chrono::seconds(10), [&] { return terminal; }));
    }
    REQUIRE_MESSAGE(ok, "download failed: " << error);
    // Engine couldn't know the size up front -> length stays -1 and we
    // shouldn't pretend the server supports ranges.
    CHECK(startedContentLength == -1);
    CHECK(startedSupportsRanges == false);
    CHECK(chunkCount == 1);
    // Bytes still flowed to disk: payload.bin is "hi" (2 bytes).
    CHECK(finalReceived == 2);
    std::ifstream f2(outputPath, std::ios::binary);
    const std::string body{(std::istreambuf_iterator<char>(f2)),
                           std::istreambuf_iterator<char>()};
    CHECK(body == "hi");
}
