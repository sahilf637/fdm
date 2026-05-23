// Verifies the single-chunk fallback path against Python's stdlib http.server,
// which intentionally does NOT advertise Accept-Ranges: bytes. The engine
// should detect this and fall back to one GET covering the whole file.
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

}  // namespace

TEST_CASE("server without Accept-Ranges -> single-chunk fallback, bytes match") {
    const fs::path tmp = fs::temp_directory_path() / "fdm_fallback_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);

    const fs::path inputPath = tmp / "input.bin";
    const fs::path outputPath = tmp / "output.bin";

    constexpr std::size_t kSize = 3 * 1024 * 1024;  // large enough to want chunking, but server won't allow it
    const std::string source = generateRandomBytes(kSize);
    std::ofstream(inputPath, std::ios::binary).write(source.data(), source.size());

    const int port = findFreePort();
    StdlibHttpServer server(tmp, port);

    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/input.bin";

    fdm::DownloadEngine engine;
    std::mutex m;
    std::condition_variable cv;
    bool terminal = false;
    bool ok = false;
    std::string error;
    int chunkCount = -1;
    bool supportsRanges = true;

    engine.start(url, outputPath.string(), [&](fdm::EngineEvent ev) {
        std::visit(
            [&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, fdm::Started>) {
                    chunkCount = e.chunkCount;
                    supportsRanges = e.supportsRanges;
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
    CHECK(supportsRanges == false);
    CHECK(chunkCount == 1);

    const std::string downloaded = readFile(outputPath);
    CHECK(downloaded.size() == kSize);
    CHECK(downloaded == source);
}
