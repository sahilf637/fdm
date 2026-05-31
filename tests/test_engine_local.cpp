#include "doctest.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <variant>

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

// Fork+exec a small Python range-supporting HTTP server (tests/range_server.py)
// on the given port serving from `root`. SIGTERM in the destructor stops it.
// Safe to fork here because the engine thread hasn't been created yet at the
// call site.
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
            ::execlp("python3", "python3", script.c_str(),
                     portStr.c_str(), rootStr.c_str(),
                     static_cast<char*>(nullptr));
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
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

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

std::string generateRandomBytes(std::size_t size) {
    std::string buf(size, '\0');
    std::mt19937_64 rng(0xC0FFEE);  // deterministic seed for reproducibility
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

}  // namespace

TEST_CASE("8 MiB local download splits into parallel segments, bytes match source") {
    const fs::path tmp = fs::temp_directory_path() / "fdm_local_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);

    const fs::path inputPath = tmp / "input.bin";
    const fs::path outputPath = tmp / "output.bin";

    constexpr std::size_t kSize = 8 * 1024 * 1024;  // 8 MiB
    const std::string source = generateRandomBytes(kSize);
    std::ofstream(inputPath, std::ios::binary).write(source.data(), source.size());

    const int port = findFreePort();
    HttpServer server(tmp, port);

    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/input.bin";

    fdm::DownloadEngine engine;

    std::mutex m;
    std::condition_variable cv;
    bool terminal = false;
    bool ok = false;
    std::string error;
    int chunkCount = 0;

    engine.start(url, outputPath.string(), [&](fdm::EngineEvent ev) {
        std::visit(
            [&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, fdm::Started>) {
                    chunkCount = e.chunkCount;
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
        REQUIRE(cv.wait_for(lk, std::chrono::seconds(30),
                            [&] { return terminal; }));
    }

    REQUIRE_MESSAGE(ok, "download failed: " << error);
    // Adaptive engine seeds kInitialConnections segments (parallel) and grows up
    // to kMaxConnections dynamically. Assert it split into multiple parallel
    // segments (not a single stream) rather than a fixed count.
    CHECK(chunkCount >= 2);
    CHECK(chunkCount <= 8);

    const std::string downloaded = readFile(outputPath);
    CHECK(downloaded.size() == kSize);
    CHECK(downloaded == source);
}
