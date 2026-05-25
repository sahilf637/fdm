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

// Drive engine.probe() and synchronously return its result.
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

struct DispositionFixture {
    fs::path tmp;
    HttpServer server;
    fdm::DownloadEngine engine;

    DispositionFixture()
        : tmp(fs::temp_directory_path() / "fdm_disposition_test"),
          server((fs::create_directories(tmp),
                  std::ofstream(tmp / "payload.bin", std::ios::binary)
                      .write("hi", 2),
                  tmp),
                 findFreePort()) {}

    std::string url(const std::string& query) const {
        return "http://127.0.0.1:" + std::to_string(server.port()) +
               "/payload.bin?dispo=" + query;
    }
};

}  // namespace

TEST_CASE("probe extracts filename from quoted Content-Disposition") {
    DispositionFixture f;
    // attachment; filename="report-2025.pdf"
    const auto r = probeSync(f.engine, f.url("attachment%3B%20filename%3D%22report-2025.pdf%22"));
    REQUIRE(r.ok);
    CHECK(r.info.suggestedFilename == "report-2025.pdf");
}

TEST_CASE("probe extracts filename from unquoted Content-Disposition") {
    DispositionFixture f;
    // attachment; filename=plain.zip
    const auto r = probeSync(f.engine, f.url("attachment%3B%20filename%3Dplain.zip"));
    REQUIRE(r.ok);
    CHECK(r.info.suggestedFilename == "plain.zip");
}

TEST_CASE("probe prefers filename* (RFC 5987) over filename") {
    DispositionFixture f;
    // attachment; filename="ascii.bin"; filename*=UTF-8''na%C3%AFve.txt
    const auto r = probeSync(f.engine, f.url(
        "attachment%3B%20filename%3D%22ascii.bin%22%3B%20"
        "filename%2A%3DUTF-8%27%27na%25C3%25AFve.txt"));
    REQUIRE(r.ok);
    CHECK(r.info.suggestedFilename == "na\xC3\xAFve.txt");
}

TEST_CASE("probe sanitizes path separators in Content-Disposition") {
    DispositionFixture f;
    // attachment; filename="../../etc/passwd"
    const auto r = probeSync(f.engine, f.url(
        "attachment%3B%20filename%3D%22..%2F..%2Fetc%2Fpasswd%22"));
    REQUIRE(r.ok);
    // Path separators stripped, dots preserved -- still safe because the
    // caller treats this as a leaf filename (will be joined onto the chosen
    // directory).
    CHECK(r.info.suggestedFilename == "....etcpasswd");
}

TEST_CASE("probe returns empty suggestedFilename when header absent") {
    DispositionFixture f;
    fdm::DownloadEngine engine;
    const std::string url =
        "http://127.0.0.1:" + std::to_string(f.server.port()) + "/payload.bin";
    const auto r = probeSync(engine, url);
    REQUIRE(r.ok);
    CHECK(r.info.suggestedFilename.empty());
}
