#include "doctest.h"

#include <curl/curl.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

#include "fdm/DownloadEngine.h"
#include "local_server_fixture.h"

namespace {

std::size_t writeToString(char* ptr, std::size_t size, std::size_t nmemb, void* userp) {
    const std::size_t total = size * nmemb;
    static_cast<std::string*>(userp)->append(ptr, total);
    return total;
}

}  // namespace

TEST_CASE("async probe returns Content-Length and Accept-Ranges") {
    const std::string source(594, 'x');  // mirrors the size we used to fetch from Ubuntu
    fdm_test::ScopedRangeServer server("fdm_probe_test", "src.bin", source);

    fdm::DownloadEngine engine;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    fdm::ProbeResult result;

    engine.probe(server.url, [&](fdm::ProbeResult r) {
        std::lock_guard<std::mutex> lk(m);
        result = std::move(r);
        done = true;
        cv.notify_one();
    });

    {
        std::unique_lock<std::mutex> lk(m);
        REQUIRE(cv.wait_for(lk, std::chrono::seconds(15), [&] { return done; }));
    }

    REQUIRE(result.ok);
    CHECK(result.info.contentLength == 594);
    CHECK(result.info.supportsRanges == true);
    CHECK(result.info.finalUrl.find("src.bin") != std::string::npos);
}

TEST_CASE("engine multi loop dispatches CURLMSG_DONE via CURLOPT_PRIVATE") {
    const std::string source = "hello-from-local-range-server";
    fdm_test::ScopedRangeServer server("fdm_multi_test", "src.bin", source);

    fdm::DownloadEngine engine;

    std::string body;
    CURLcode finalRc = CURLE_OK;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;

    CURL* easy = curl_easy_init();
    REQUIRE(easy != nullptr);
    curl_easy_setopt(easy, CURLOPT_URL, server.url.c_str());
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &body);

    auto ctx = std::make_unique<fdm::EasyContext>();
    ctx->onDone = [&](CURLcode rc) {
        std::lock_guard<std::mutex> lk(m);
        finalRc = rc;
        done = true;
        cv.notify_one();
    };
    curl_easy_setopt(easy, CURLOPT_PRIVATE, ctx.get());

    engine.addEasy(easy);

    {
        std::unique_lock<std::mutex> lk(m);
        REQUIRE(cv.wait_for(lk, std::chrono::seconds(15), [&] { return done; }));
    }

    CHECK(finalRc == CURLE_OK);
    CHECK(body == source);

    curl_easy_cleanup(easy);
}
