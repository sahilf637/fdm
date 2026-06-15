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

struct Outcome {
    bool ok = false;
    bool terminal = false;
    std::string error;
};

// Drive engine.start() to its terminal event. Returns once Finished/Failed.
Outcome runDownload(fdm::DownloadEngine& engine, const std::string& url,
                    const std::string& out, std::chrono::seconds timeout) {
    std::mutex m;
    std::condition_variable cv;
    Outcome res;
    engine.start(url, out, [&](fdm::EngineEvent ev) {
        std::visit(
            [&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, fdm::Finished>) {
                    std::lock_guard<std::mutex> lk(m);
                    res.ok = true;
                    res.terminal = true;
                    cv.notify_one();
                } else if constexpr (std::is_same_v<T, fdm::Failed>) {
                    std::lock_guard<std::mutex> lk(m);
                    res.error = e.reason;
                    res.terminal = true;
                    cv.notify_one();
                }
            },
            ev);
    });
    std::unique_lock<std::mutex> lk(m);
    cv.wait_for(lk, timeout, [&] { return res.terminal; });
    return res;
}

}  // namespace

TEST_CASE("rate-limited server: download still completes despite 429s") {
    const fs::path root = fs::temp_directory_path() / "fdm_limit_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    // 6 MiB of deterministic bytes -> splits into multiple segments, so the
    // engine opens more connections than the server's limit (2) allows.
    std::string body(6 * 1024 * 1024, '\0');
    for (std::size_t i = 0; i < body.size(); ++i)
        body[i] = static_cast<char>((i * 131 + 7) & 0xff);
    std::ofstream(root / "file.bin", std::ios::binary).write(body.data(), body.size());

    fdm_test::PythonServer server(
        std::string(FDM_TEST_SOURCE_DIR) + "/limit_server.py", root,
        fdm_test::findFreePort());
    const std::string base = "http://127.0.0.1:" + std::to_string(server.port());

    fdm::DownloadEngine engine;

    SUBCASE("completes and the bytes are correct") {
        const fs::path out = root / "out.bin";
        const Outcome r = runDownload(engine, base + "/file.bin", out.string(),
                                      std::chrono::seconds(60));
        REQUIRE(r.terminal);
        REQUIRE(r.ok);  // the old engine failed the whole download here
        CHECK(readFile(out) == body);

        // The server never served more than its concurrency limit.
        const std::string peak = readFile(root / "maxconc.txt");
        if (!peak.empty()) CHECK(std::stoi(peak) <= 2);
    }

    SUBCASE("a 404 fails fast (fatal, not retried forever)") {
        const fs::path out = root / "missing.out";
        const Outcome r = runDownload(engine, base + "/does-not-exist.bin",
                                      out.string(), std::chrono::seconds(20));
        REQUIRE(r.terminal);
        CHECK_FALSE(r.ok);
    }
}

TEST_CASE("hard-limited server: 403 on extra connections doesn't kill the download") {
    const fs::path root = fs::temp_directory_path() / "fdm_limit403_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    // 6 MiB -> the engine opens more connections than the server's limit (2),
    // which this server rejects with 403 instead of 429. The rejected
    // segments must be retried (and the cap backed off), not treated as a
    // fatal error for the whole download.
    std::string body(6 * 1024 * 1024, '\0');
    for (std::size_t i = 0; i < body.size(); ++i)
        body[i] = static_cast<char>((i * 173 + 11) & 0xff);
    std::ofstream(root / "file.bin", std::ios::binary).write(body.data(), body.size());

    fdm_test::PythonServer server(
        std::string(FDM_TEST_SOURCE_DIR) + "/limit403_server.py", root,
        fdm_test::findFreePort());
    const std::string base = "http://127.0.0.1:" + std::to_string(server.port());

    fdm::DownloadEngine engine;

    SUBCASE("completes and the bytes are correct") {
        const fs::path out = root / "out.bin";
        const Outcome r = runDownload(engine, base + "/file.bin", out.string(),
                                      std::chrono::seconds(60));
        REQUIRE(r.terminal);
        REQUIRE(r.ok);
        CHECK(readFile(out) == body);

        const std::string peak = readFile(root / "maxconc.txt");
        if (!peak.empty()) CHECK(std::stoi(peak) <= 2);
    }

    SUBCASE("a 404 still fails fast") {
        const fs::path out = root / "missing.out";
        const Outcome r = runDownload(engine, base + "/does-not-exist.bin",
                                      out.string(), std::chrono::seconds(20));
        REQUIRE(r.terminal);
        CHECK_FALSE(r.ok);
    }
}
