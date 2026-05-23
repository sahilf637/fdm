#include "doctest.h"

#include <curl/curl.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>

#include "ChunkTask.h"
#include "fdm/ChunkSpec.h"
#include "fdm/DownloadEngine.h"
#include "local_server_fixture.h"

namespace fs = std::filesystem;

TEST_CASE("ChunkTask downloads a single Range to disk at the right offset") {
    // Self-contained: 1 KiB of deterministic bytes served by the local range
    // server. No internet needed.
    std::string source(1024, '\0');
    for (std::size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<char>((i * 31 + 7) & 0xff);
    }

    fdm_test::ScopedRangeServer server("fdm_chunk_task_test", "src.bin", source);
    const fs::path outPath = server.root / "out.bin";
    std::error_code ec;
    fs::remove(outPath, ec);

    fdm::DownloadEngine engine;
    const fdm::ChunkSpec spec{0, 255, 0};
    fdm::ChunkTask task(server.url, outPath.string(), spec);

    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    CURLcode finalRc = CURLE_OK;

    task.start(engine, [&](CURLcode rc) {
        std::lock_guard<std::mutex> lk(m);
        finalRc = rc;
        done = true;
        cv.notify_one();
    });

    {
        std::unique_lock<std::mutex> lk(m);
        REQUIRE(cv.wait_for(lk, std::chrono::seconds(15), [&] { return done; }));
    }

    CHECK(finalRc == CURLE_OK);
    CHECK(task.bytesWritten() == 256);

    std::ifstream f(outPath, std::ios::binary);
    const std::string ours((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    CHECK(ours.size() == 256);
    CHECK(ours == source.substr(0, 256));
}
