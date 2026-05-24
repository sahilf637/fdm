#include <curl/curl.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <variant>

#include "fdm/DownloadEngine.h"
#include "fdm/Paths.h"

namespace {

int runProbe(const char* url) {
    fdm::DownloadEngine engine;

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

    {
        std::unique_lock<std::mutex> lk(m);
        if (!cv.wait_for(lk, std::chrono::seconds(60), [&] { return done; })) {
            std::fprintf(stderr, "probe timeout\n");
            return 1;
        }
    }

    if (!result.ok) {
        std::fprintf(stderr, "probe failed: %s\n", result.error.c_str());
        return 1;
    }
    std::printf("url:             %s\n", result.info.finalUrl.c_str());
    std::printf("content-length:  %lld\n", static_cast<long long>(result.info.contentLength));
    std::printf("supports-ranges: %s\n", result.info.supportsRanges ? "yes" : "no");
    return 0;
}

// Pull the trailing path segment off a URL (after stripping ?query / #fragment).
// Returns empty string if the URL has no usable basename.
std::string filenameFromUrl(const std::string& url) {
    const std::size_t end = url.find_first_of("?#");
    const std::string clean = (end == std::string::npos) ? url : url.substr(0, end);
    const std::size_t slash = clean.rfind('/');
    if (slash == std::string::npos) return {};
    return clean.substr(slash + 1);
}

int runDownload(const char* url, const char* outputPath) {
    namespace fs = std::filesystem;
    fs::path out(outputPath);
    if (fs::is_directory(out)) {
        std::string name = filenameFromUrl(url);
        if (name.empty()) name = "download.bin";
        out /= name;
    }
    const std::string finalPath = fdm::findAvailablePath(out.string());
    if (finalPath != out.string()) {
        std::fprintf(stderr, "file exists, saving as %s\n", finalPath.c_str());
    } else if (out.string() != outputPath) {
        std::fprintf(stderr, "writing to %s\n", finalPath.c_str());
    }
    out = finalPath;

    fdm::DownloadEngine engine;

    std::mutex m;
    std::condition_variable cv;
    bool terminal = false;
    bool ok = false;
    std::string error;

    engine.start(url, out.string(), [&](fdm::EngineEvent ev) {
        std::visit(
            [&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, fdm::Started>) {
                    std::fprintf(stderr,
                                 "[started] size=%lld ranges=%s chunks=%d\n",
                                 static_cast<long long>(e.contentLength),
                                 e.supportsRanges ? "yes" : "no",
                                 e.chunkCount);
                } else if constexpr (std::is_same_v<T, fdm::Progress>) {
                    const double mbps = e.bytesPerSec / (1024.0 * 1024.0);
                    if (e.total > 0) {
                        const double pct = 100.0 * static_cast<double>(e.received) /
                                           static_cast<double>(e.total);
                        std::fprintf(stderr,
                                     "\r[progress] %lld / %lld bytes (%.1f%%) - %.2f MiB/s   ",
                                     static_cast<long long>(e.received),
                                     static_cast<long long>(e.total), pct, mbps);
                    } else {
                        std::fprintf(stderr,
                                     "\r[progress] %lld bytes - %.2f MiB/s   ",
                                     static_cast<long long>(e.received), mbps);
                    }
                    std::fflush(stderr);
                } else if constexpr (std::is_same_v<T, fdm::Finished>) {
                    std::fprintf(stderr, "\n[finished]\n");
                    std::lock_guard<std::mutex> lk(m);
                    ok = true;
                    terminal = true;
                    cv.notify_one();
                } else if constexpr (std::is_same_v<T, fdm::Failed>) {
                    std::fprintf(stderr, "\n[failed] %s\n", e.reason.c_str());
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
        cv.wait(lk, [&] { return terminal; });
    }
    return ok ? 0 : 1;
}

void printUsage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  fdm-cli                        print libcurl version\n"
                 "  fdm-cli --probe <url>          HEAD-probe a URL\n"
                 "  fdm-cli <url> <output-path>    download a URL to a file\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        std::fprintf(stderr, "curl_global_init failed\n");
        return 1;
    }

    int rc = 0;
    if (argc == 1) {
        std::printf("%s\n", fdm::engineVersion().c_str());
    } else if (argc == 3 && std::strcmp(argv[1], "--probe") == 0) {
        rc = runProbe(argv[2]);
    } else if (argc == 3) {
        rc = runDownload(argv[1], argv[2]);
    } else {
        printUsage();
        rc = 2;
    }

    curl_global_cleanup();
    return rc;
}
