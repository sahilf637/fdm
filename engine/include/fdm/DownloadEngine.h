#pragma once

#include <curl/curl.h>

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fdm/ChunkSpec.h"
#include "fdm/DownloadInfo.h"
#include "fdm/EngineEvent.h"

namespace fdm {

std::string engineVersion();

// Opaque, monotonically increasing id used by callers to address a single
// download for pause/resume/cancel. The engine's `start()` / `resumeKnown()`
// returns one; values are unique within a single DownloadEngine instance.
using DownloadId = std::int64_t;

struct ProbeResult {
    bool ok = false;
    DownloadInfo info;
    std::string error;
};

// Per-chunk state for restoring a previously-interrupted download. Together
// with the URL, output path, and total size this is everything the engine
// needs to skip the probe/split step and pick up exactly where the last run
// stopped.
struct ChunkRestore {
    int index = 0;
    std::int64_t startByte = 0;
    std::int64_t endByte = -1;
    std::int64_t bytesReceived = 0;
    int attempts = 1;
    ChunkProgress::Status status = ChunkProgress::Status::Active;
};

struct ResumeSpec {
    std::string url;
    std::string outputPath;
    std::int64_t totalBytes = -1;
    bool supportsRanges = true;
    std::vector<ChunkRestore> chunks;
    // Extra request headers (raw "Name: value" lines) replayed on every chunk
    // so an authenticated download (cookies, referer) resumes the same way it
    // started.
    std::vector<std::string> headers;
    // If-Range validator captured by the original probe (see
    // Started::validator). When non-empty it is sent on every ranged chunk
    // request; a changed resource then returns 200 and the download fails
    // instead of corrupting the file. Empty = resume without protection.
    std::string validator;
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

    // Shared DNS / TLS-session / connection cache applied to every easy handle
    // (probe + chunks). Lets parallel chunks resume TLS sessions and reuse
    // connections instead of each paying a fresh handshake. Owned by the engine.
    CURLSH* shareHandle() const { return share_; }

    // HEAD-request a URL on the engine thread. onResult fires (on the engine
    // thread) once the probe completes.
    void probe(std::string url, std::function<void(ProbeResult)> onResult);

    // As above, but sends extra request headers (raw "Name: value" lines) --
    // e.g. Cookie / Referer / User-Agent forwarded from a browser.
    void probe(std::string url, std::vector<std::string> headers,
               std::function<void(ProbeResult)> onResult);

    // Start a chunked download. Emits Started, then Progress events, then
    // either Finished or Failed exactly once. All events fire on the engine
    // thread. Pre-allocates the output file when Content-Length is known.
    // Returns the engine-assigned id used to address this download for
    // pause/resume/cancel.
    DownloadId start(std::string url,
                     std::string outputPath,
                     std::function<void(EngineEvent)> onEvent);

    // As above, but sends extra request headers (raw "Name: value" lines) on
    // the probe and on every chunk request.
    DownloadId start(std::string url,
                     std::string outputPath,
                     std::vector<std::string> headers,
                     std::function<void(EngineEvent)> onEvent);

    // Like start(), but skips probing and chunk-splitting -- the caller
    // already knows the URL's metadata and each chunk's progress (typically
    // from persistent storage). Behaves identically once running: emits
    // Started, then Progress, then Finished or Failed exactly once.
    DownloadId resumeKnown(ResumeSpec spec,
                           std::function<void(EngineEvent)> onEvent);

    // Pause one in-flight download (no-op if id is unknown / not active).
    // Emits Paused. Chunks are removed from libcurl multi and any pending
    // retries dropped.
    void pause(DownloadId id);

    // Resume a paused download in-process (no-op if not paused).
    void resume(DownloadId id);

    // Cancel one in-flight download. Emits Failed{"Cancelled"} and tears
    // down. Does NOT touch the partial file on disk.
    void cancel(DownloadId id);

    // Implementation-only nested types. Forward-declared here so private
    // members can reference them; full definitions live in DownloadEngine.cpp
    // and are opaque to anyone including this header.
    struct PendingRetry;
    struct DownloadState;
    struct Segment;

private:
    void runLoop();
    DownloadState* findById(DownloadId id);

    // --- segment scheduler (engine-thread-only) ------------------------------
    // Set up a fresh download's initial segments from `chunks`, emit Started,
    // register it, and start pumping. `restoreOverlay` carries per-segment
    // resume state (nullptr for a brand-new download).
    void beginDownload(DownloadState* state, const std::vector<ChunkSpec>& chunks,
                       const std::vector<ChunkRestore>* restoreOverlay);
    // Fill free connection slots: start Pending segments whose backoff has
    // elapsed, and split the largest in-flight segment when nothing is pending.
    void pump(DownloadState* state);
    void startSegment(DownloadState* state, Segment* seg);
    Segment* findSegment(DownloadState* state, int index);
    Segment* trySplitLargestActive(DownloadState* state);
    // Per-segment terminal handler invoked from the curl completion callback.
    void onSegmentDone(DownloadState* state, int segIndex, CURLcode rc, long http,
                       bool capped);
    void maybeFinish(DownloadState* state);   // emit Finished when all segments Done
    void failDownload(DownloadState* state, const std::string& reason);

    CURLM* multi_ = nullptr;
    CURLSH* share_ = nullptr;
    std::thread thread_;
    std::mutex mu_;
    std::deque<std::function<void()>> commands_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<DownloadId> nextId_{1};
    std::vector<PendingRetry> retries_;       // engine-thread-only
    std::vector<DownloadState*> activeStates_;  // engine-thread-only
};

}  // namespace fdm
