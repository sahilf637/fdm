// Regression tests for resuming an open-ended download (the server did not
// advertise Accept-Ranges, so the chunk has endByte == -1) with bytes already
// on disk. The engine sends a best-effort Range for the tail and must either
// continue from the resume offset (server honors it, 206) or rewrite from the
// start (server ignores it, 200) -- never append the full body at the resume
// offset, which corrupts the file.
#include "doctest.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <string>
#include <variant>

#include "fdm/DownloadEngine.h"
#include "local_server_fixture.h"

namespace fs = std::filesystem;

namespace {

std::string generateRandomBytes(std::size_t size) {
    std::string buf(size, '\0');
    std::mt19937_64 rng(0xDEADBEEF);
    auto* data = reinterpret_cast<std::uint64_t*>(buf.data());
    const std::size_t words = size / 8;
    for (std::size_t i = 0; i < words; ++i) data[i] = rng();
    for (std::size_t i = words * 8; i < size; ++i) {
        buf[i] = static_cast<char>(rng() & 0xff);
    }
    return buf;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

// Python's stdlib http.server: serves files but ignores Range requests
// (always replies 200 with the full body) and never sends Accept-Ranges.
class StdlibHttpServer {
public:
    StdlibHttpServer(const fs::path& root, int port) : port_(port) {
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
            ::execlp("python3", "python3", "-m", "http.server",
                     portStr.c_str(), "--bind", "127.0.0.1",
                     "--directory", rootStr.c_str(),
                     static_cast<char*>(nullptr));
            ::_exit(127);
        }
        waitForPort();
    }
    ~StdlibHttpServer() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
    }
    StdlibHttpServer(const StdlibHttpServer&) = delete;
    StdlibHttpServer& operator=(const StdlibHttpServer&) = delete;

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

// A ResumeSpec describing one open-ended chunk with `resumed` bytes on disk.
fdm::ResumeSpec openEndedSpec(const std::string& url, const fs::path& out,
                              std::int64_t totalBytes, std::int64_t resumed) {
    fdm::ResumeSpec spec;
    spec.url = url;
    spec.outputPath = out.string();
    spec.totalBytes = totalBytes;
    spec.supportsRanges = false;
    fdm::ChunkRestore r;
    r.index = 0;
    r.startByte = 0;
    r.endByte = -1;
    r.bytesReceived = resumed;
    spec.chunks.push_back(r);
    return spec;
}

// Run resumeKnown to a terminal event. Returns true on Finished; on Failed
// fills `error`.
bool runResume(fdm::DownloadEngine& engine, fdm::ResumeSpec spec, std::string* error) {
    std::mutex m;
    std::condition_variable cv;
    bool terminal = false;
    bool ok = false;
    engine.resumeKnown(std::move(spec), [&](fdm::EngineEvent ev) {
        std::visit(
            [&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, fdm::Finished>) {
                    std::lock_guard<std::mutex> lk(m);
                    ok = true;
                    terminal = true;
                    cv.notify_one();
                } else if constexpr (std::is_same_v<T, fdm::Failed>) {
                    std::lock_guard<std::mutex> lk(m);
                    *error = e.reason;
                    terminal = true;
                    cv.notify_one();
                }
            },
            ev);
    });
    std::unique_lock<std::mutex> lk(m);
    if (!cv.wait_for(lk, std::chrono::seconds(30), [&] { return terminal; })) {
        *error = "timed out";
        return false;
    }
    return ok;
}

// Shared scenario: seed the output file with the first `resumed` bytes of
// `source`, then resume the open-ended download from `url` and verify the
// final file is byte-identical to the source.
void checkOpenEndedResume(const fs::path& outputPath, const std::string& source,
                          std::size_t resumed, const std::string& url) {
    std::ofstream(outputPath, std::ios::binary)
        .write(source.data(), static_cast<std::streamsize>(resumed));

    fdm::DownloadEngine engine;
    std::string error;
    const bool ok = runResume(
        engine,
        openEndedSpec(url, outputPath, static_cast<std::int64_t>(source.size()),
                      static_cast<std::int64_t>(resumed)),
        &error);
    REQUIRE_MESSAGE(ok, "resume failed: " << error);

    const std::string downloaded = readFile(outputPath);
    CHECK(downloaded.size() == source.size());
    CHECK(downloaded == source);
}

}  // namespace

TEST_CASE("open-ended resume, server ignores Range (200) -> rewrites from start") {
    const fs::path tmp = fs::temp_directory_path() / "fdm_resume_200_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);

    constexpr std::size_t kSize = 2 * 1024 * 1024;
    constexpr std::size_t kResumed = 700 * 1024;
    const std::string source = generateRandomBytes(kSize);
    std::ofstream(tmp / "input.bin", std::ios::binary)
        .write(source.data(), source.size());

    const int port = fdm_test::findFreePort();
    StdlibHttpServer server(tmp, port);
    const std::string url =
        "http://127.0.0.1:" + std::to_string(port) + "/input.bin";

    checkOpenEndedResume(tmp / "output.bin", source, kResumed, url);
}

TEST_CASE("open-ended resume, server honors Range (206) -> continues from offset") {
    constexpr std::size_t kSize = 2 * 1024 * 1024;
    constexpr std::size_t kResumed = 700 * 1024;
    const std::string source = generateRandomBytes(kSize);

    fdm_test::ScopedRangeServer server("fdm_resume_206_test", "input.bin", source);

    checkOpenEndedResume(server.root / "output.bin", source, kResumed, server.url);
}
