#include "doctest.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <variant>

#include "fdm/DownloadEngine.h"
#include "local_server_fixture.h"

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("engine forwards caller-supplied headers onto every request") {
    // Server that records the request headers it sees into received_headers.txt.
    namespace fsx = std::filesystem;
    const fs::path root = fsx::temp_directory_path() / "fdm_header_fwd_test";
    std::error_code ec;
    fsx::remove_all(root, ec);
    fsx::create_directories(root);
    const std::string body(64 * 1024, 'A');  // big enough to split into chunks
    std::ofstream(root / "file.bin", std::ios::binary).write(body.data(), body.size());

    fdm_test::PythonServer server(
        std::string(FDM_TEST_SOURCE_DIR) + "/header_echo_server.py", root,
        fdm_test::findFreePort());
    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/file.bin";
    const fs::path out = root / "out.bin";

    fdm::DownloadEngine engine;
    std::mutex m;
    std::condition_variable cv;
    bool terminal = false;
    bool ok = false;

    const std::vector<std::string> headers = {
        "Cookie: session=secret-token",
        "Referer: https://example.com/page",
    };

    engine.start(url, out.string(), headers, [&](fdm::EngineEvent ev) {
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
                    terminal = true;
                    cv.notify_one();
                }
            },
            ev);
    });

    {
        std::unique_lock<std::mutex> lk(m);
        REQUIRE(cv.wait_for(lk, std::chrono::seconds(30), [&] { return terminal; }));
    }
    REQUIRE(ok);
    CHECK(readFile(out) == body);

    const std::string seen = readFile(root / "received_headers.txt");
    CHECK(seen.find("Cookie: session=secret-token") != std::string::npos);
    CHECK(seen.find("Referer: https://example.com/page") != std::string::npos);
}
