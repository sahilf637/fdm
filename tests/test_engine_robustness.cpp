// Robustness tests: cover the failure modes a real internet connection throws
// at us -- servers that lie about range support, servers that take too long
// to respond, etc.
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
#include <cstdint>
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

class PythonServer {
public:
    PythonServer(const std::string& script, const fs::path& root, int port)
        : port_(port) {
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
    ~PythonServer() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
    }
    PythonServer(const PythonServer&) = delete;
    PythonServer& operator=(const PythonServer&) = delete;

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

std::string randomBytes(std::size_t size, std::uint64_t seed) {
    std::string buf(size, '\0');
    std::mt19937_64 rng(seed);
    auto* data = reinterpret_cast<std::uint64_t*>(buf.data());
    const std::size_t words = size / 8;
    for (std::size_t i = 0; i < words; ++i) data[i] = rng();
    for (std::size_t i = words * 8; i < size; ++i) {
        buf[i] = static_cast<char>(rng() & 0xff);
    }
    return buf;
}

struct DownloadOutcome {
    bool ok = false;
    std::string error;
    int chunkCount = 0;
};

DownloadOutcome runDownload(const std::string& url, const fs::path& output) {
    fdm::DownloadEngine engine;
    std::mutex m;
    std::condition_variable cv;
    bool terminal = false;
    DownloadOutcome outcome;

    engine.start(url, output.string(), [&](fdm::EngineEvent ev) {
        std::visit(
            [&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, fdm::Started>) {
                    outcome.chunkCount = e.chunkCount;
                } else if constexpr (std::is_same_v<T, fdm::Finished>) {
                    std::lock_guard<std::mutex> lk(m);
                    outcome.ok = true;
                    terminal = true;
                    cv.notify_one();
                } else if constexpr (std::is_same_v<T, fdm::Failed>) {
                    std::lock_guard<std::mutex> lk(m);
                    outcome.error = e.reason;
                    terminal = true;
                    cv.notify_one();
                }
            },
            ev);
    });

    std::unique_lock<std::mutex> lk(m);
    cv.wait_for(lk, std::chrono::seconds(60), [&] { return terminal; });
    return outcome;
}

}  // namespace

TEST_CASE("server that lies about Range support fails cleanly, no file corruption") {
    const fs::path tmp = fs::temp_directory_path() / "fdm_lying_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);

    constexpr std::size_t kSize = 4 * 1024 * 1024;  // 4 MiB -> 4 chunks
    const std::string source = randomBytes(kSize, 0xFEEDFACE);
    std::ofstream(tmp / "input.bin", std::ios::binary)
        .write(source.data(), source.size());

    const int port = findFreePort();
    const std::string script = std::string(FDM_TEST_SOURCE_DIR) + "/lying_server.py";
    PythonServer server(script, tmp, port);

    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/input.bin";
    const auto outcome = runDownload(url, tmp / "output.bin");

    // The download must fail (not silently produce a corrupt file). The exact
    // error message can vary by libcurl version; we just assert it's an error.
    CHECK_FALSE(outcome.ok);
    CHECK_FALSE(outcome.error.empty());
}

TEST_CASE("dead server (connection refused) reports a clear error after retries") {
    // Find a port and DON'T start a server on it.
    const int port = findFreePort();
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/file";

    const fs::path tmp = fs::temp_directory_path() / "fdm_dead_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);

    const auto outcome = runDownload(url, tmp / "output.bin");
    CHECK_FALSE(outcome.ok);
    CHECK_FALSE(outcome.error.empty());
}
