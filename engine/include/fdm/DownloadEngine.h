#pragma once

#include <curl/curl.h>

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fdm/DownloadInfo.h"
#include "fdm/EngineEvent.h"

namespace fdm {

std::string engineVersion();

struct ProbeResult {
    bool ok = false;
    DownloadInfo info;
    std::string error;
};

// Attached to a curl easy handle via CURLOPT_PRIVATE. The engine reads this
// when CURLMSG_DONE fires and invokes onDone(result). Ownership of the context
// and the easy handle stays with the caller -- onDone is responsible for
// cleanup (or for re-adding the handle to retry).
struct EasyContext {
    std::function<void(CURLcode)> onDone;
};

// Owns one std::thread and one CURLM*. All libcurl handles are touched only
// from the engine thread; the public methods are thread-safe entry points that
// queue work and wake the engine via curl_multi_wakeup.
//
// All user-supplied callbacks (probe onResult, start onEvent) fire on the
// engine thread. The caller is responsible for marshaling to their own thread
// (e.g., Qt UI via QMetaObject::invokeMethod with Qt::QueuedConnection).
class DownloadEngine {
public:
    DownloadEngine();
    ~DownloadEngine();
    DownloadEngine(const DownloadEngine&) = delete;
    DownloadEngine& operator=(const DownloadEngine&) = delete;

    // Run a command on the engine thread. Thread-safe.
    void post(std::function<void()> cmd);

    // Add a configured easy handle to the multi context. The caller must have
    // set CURLOPT_PRIVATE to an EasyContext* (or nullptr).
    void addEasy(CURL* easy);

    // HEAD-request a URL on the engine thread. onResult fires (on the engine
    // thread) once the probe completes.
    void probe(std::string url, std::function<void(ProbeResult)> onResult);

    // Start a chunked download. Emits Started, then Progress events, then
    // either Finished or Failed exactly once. All events fire on the engine
    // thread. Pre-allocates the output file when Content-Length is known.
    void start(std::string url,
               std::string outputPath,
               std::function<void(EngineEvent)> onEvent);

    // Implementation-only nested types. Forward-declared here so private
    // members can reference them; full definitions live in DownloadEngine.cpp
    // and are opaque to anyone including this header.
    struct PendingRetry;
    struct DownloadState;

private:
    void runLoop();

    CURLM* multi_ = nullptr;
    std::thread thread_;
    std::mutex mu_;
    std::deque<std::function<void()>> commands_;
    std::atomic<bool> stopRequested_{false};
    std::vector<PendingRetry> retries_;       // engine-thread-only
    std::vector<DownloadState*> activeStates_;  // engine-thread-only
};

}  // namespace fdm
