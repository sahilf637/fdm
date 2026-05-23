// Shared test fixture: spins up the range-supporting Python server on a free
// port in a temp directory with arbitrary file contents. Self-contained --
// any test using this is independent of internet connectivity.
#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fdm_test {

inline int findFreePort() {
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

// Forks python3 with `script`, port, and root. SIGTERM on dtor. Safe to fork
// here because users construct this before constructing the engine, so no
// other thread exists yet.
class PythonServer {
public:
    PythonServer(const std::string& script, const std::filesystem::path& root,
                 int port)
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

// Convenience: start a range-supporting server in a fresh temp directory
// containing `filename` -> `contents`.
struct ScopedRangeServer {
    std::filesystem::path root;
    std::string url;
    PythonServer server;

    ScopedRangeServer(const std::string& dirName, const std::string& filename,
                      const std::string& contents)
        : root(makeRoot(dirName, filename, contents)),
          server(std::string(FDM_TEST_SOURCE_DIR) + "/range_server.py",
                 root,
                 findFreePort()) {
        url = "http://127.0.0.1:" + std::to_string(server.port()) + "/" + filename;
    }

private:
    static std::filesystem::path makeRoot(const std::string& dirName,
                                          const std::string& filename,
                                          const std::string& contents) {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() / dirName;
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root);
        std::ofstream(root / filename, std::ios::binary)
            .write(contents.data(), contents.size());
        return root;
    }
};

}  // namespace fdm_test
