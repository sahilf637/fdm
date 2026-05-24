#include "fdm/DownloadEngine.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ChunkTask.h"
#include "fdm/ChunkSpec.h"

namespace fdm {

std::string engineVersion() {
    return curl_version();
}

namespace {

constexpr int kMaxChunks = 8;
constexpr std::int64_t kMinChunkSize = 1024 * 1024;  // 1 MiB
constexpr std::chrono::milliseconds kProgressInterval{100};
constexpr int kMaxProbeAttempts = 4;  // total attempts including the first

// Classify a transfer outcome as retryable (transient) or fatal. Driven by
// libcurl error code first; for FAILONERROR-triggered HTTP errors the HTTP
// status code is the source of truth.
enum class RetryDecision { Retry, Fatal };

RetryDecision classifyError(CURLcode rc, long httpCode) {
    switch (rc) {
        case CURLE_OK:
        case CURLE_HTTP_RETURNED_ERROR:
            // Fall through to HTTP status code classification below.
            break;
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_CONNECT:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_GOT_NOTHING:
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_PARTIAL_FILE:
        case CURLE_HTTP2_STREAM:
            return RetryDecision::Retry;
        default:
            // TLS failures, malformed URL, write errors etc. -- don't retry.
            return RetryDecision::Fatal;
    }
    // Transient HTTP responses worth retrying:
    //  408 Request Timeout, 425 Too Early, 429 Too Many Requests, all 5xx.
    if (httpCode == 408 || httpCode == 425 || httpCode == 429) return RetryDecision::Retry;
    if (httpCode >= 500 && httpCode < 600) return RetryDecision::Retry;
    if (httpCode >= 400) return RetryDecision::Fatal;
    return RetryDecision::Retry;  // unknown combo -- be generous
}

// Backoff schedule, exponential and bounded:
//   retry #1 -> 500 ms, #2 -> 1 s, #3 -> 2 s, #4+ -> 4 s (cap).
std::chrono::milliseconds backoffFor(int retryAttempt) {
    const int shift = std::clamp(retryAttempt - 1, 0, 3);
    return std::chrono::milliseconds(500 * (1 << shift));
}

// Friendlier error string than curl_easy_strerror for HTTP-status failures.
std::string describeError(CURLcode rc, long httpCode) {
    if (rc == CURLE_HTTP_RETURNED_ERROR && httpCode > 0) {
        return "HTTP " + std::to_string(httpCode);
    }
    if (rc == CURLE_OK && httpCode >= 400) {
        return "HTTP " + std::to_string(httpCode);
    }
    return curl_easy_strerror(rc);
}

struct HeaderState {
    std::int64_t contentLength = -1;       // populated from "Content-Length:" (200 path)
    std::int64_t contentRangeTotal = -1;   // populated from "Content-Range: bytes A-B/TOTAL" (206 path)
    bool acceptRanges = false;             // populated from "Accept-Ranges: bytes"
};

bool startsWithCI(const char* s, std::size_t slen, const char* prefix) {
    const std::size_t plen = std::strlen(prefix);
    if (slen < plen) return false;
    return strncasecmp(s, prefix, plen) == 0;
}

bool equalsCI(const char* s, std::size_t slen, const char* literal) {
    const std::size_t llen = std::strlen(literal);
    if (slen != llen) return false;
    return strncasecmp(s, literal, llen) == 0;
}

std::size_t headerCallback(char* buf, std::size_t size, std::size_t nmemb, void* userp) {
    const std::size_t total = size * nmemb;
    auto* st = static_cast<HeaderState*>(userp);

    std::size_t len = total;
    while (len && (buf[len - 1] == '\r' || buf[len - 1] == '\n')) --len;

    auto skipValueWhitespace = [&](const char* p, const char* end) {
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
        return p;
    };

    if (startsWithCI(buf, len, "content-length:")) {
        const char* p = skipValueWhitespace(buf + std::strlen("content-length:"), buf + len);
        st->contentLength = std::strtoll(p, nullptr, 10);
    } else if (startsWithCI(buf, len, "accept-ranges:")) {
        const char* p = skipValueWhitespace(buf + std::strlen("accept-ranges:"), buf + len);
        if (equalsCI(p, len - (p - buf), "bytes")) {
            st->acceptRanges = true;
        }
    } else if (startsWithCI(buf, len, "content-range:")) {
        // Format: "Content-Range: bytes 0-0/12345" or ".../*"
        const char* p = skipValueWhitespace(buf + std::strlen("content-range:"), buf + len);
        const char* end = buf + len;
        if (end - p >= 5 && strncasecmp(p, "bytes", 5) == 0) {
            p += 5;
            while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
        }
        while (p < end && *p != '/') ++p;
        if (p < end && *p == '/') {
            ++p;
            if (p < end && *p != '*') {
                st->contentRangeTotal = std::strtoll(p, nullptr, 10);
            }
        }
    }

    return total;
}

struct ProbeState {
    HeaderState header;
    CURL* easy = nullptr;
    EasyContext ctx;
    std::function<void(ProbeResult)> onResult;
    int attempts = 1;
    std::string url;  // kept for diagnostic / re-issue purposes
};

// Write callback for probe: discards the byte we got from "Range: bytes=0-0"
// when the server honors the range (206). If the server ignored the Range
// header and started streaming the full body (200), returning 0 here makes
// libcurl abort with CURLE_WRITE_ERROR so we don't accidentally download the
// whole file just to learn its size.
std::size_t probeDiscardWrite(char*, std::size_t size, std::size_t nmemb, void* userp) {
    auto* state = static_cast<ProbeState*>(userp);
    long http = 0;
    curl_easy_getinfo(state->easy, CURLINFO_RESPONSE_CODE, &http);
    if (http != 206) return 0;  // abort full-body downloads
    return size * nmemb;
}

}  // namespace

// One in-flight download. Created when start() is called, deleted after the
// terminal event (Finished or Failed) fires. Touched only from engine thread.
struct DownloadEngine::DownloadState {
    DownloadId id = 0;
    std::string url;
    std::string outputPath;
    std::function<void(EngineEvent)> onEvent;
    std::int64_t totalBytes = -1;
    bool supportsRanges = false;
    int totalChunks = 0;
    int completedChunks = 0;
    int failedChunks = 0;
    std::string firstError;
    std::vector<std::unique_ptr<ChunkTask>> tasks;
    std::vector<ChunkProgress::Status> chunkStatuses;
    std::chrono::steady_clock::time_point lastProgressEmit{};
    std::int64_t lastEmitReceived = 0;
    bool paused = false;

    void emit(EngineEvent ev) {
        if (onEvent) onEvent(std::move(ev));
    }
};

namespace {

// Build the per-chunk snapshot included in every Progress event.
std::vector<ChunkProgress> snapshotChunks(const DownloadEngine::DownloadState& state) {
    std::vector<ChunkProgress> out;
    out.reserve(state.tasks.size());
    for (std::size_t i = 0; i < state.tasks.size(); ++i) {
        const auto& t = state.tasks[i];
        ChunkProgress cp;
        cp.index = t->spec().index;
        cp.startByte = t->spec().startByte;
        cp.endByte = t->spec().endByte;
        cp.bytesReceived = t->bytesWritten();
        cp.attempts = t->attempts();
        cp.status = state.chunkStatuses[i];
        out.push_back(cp);
    }
    return out;
}

// Compute the byte rate since the previous Progress emit. Returns 0 on the
// first call (no baseline yet).
double computeBytesPerSec(DownloadEngine::DownloadState& state, std::int64_t received,
                          std::chrono::steady_clock::time_point now) {
    double rate = 0.0;
    if (state.lastProgressEmit.time_since_epoch().count() != 0) {
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                                now - state.lastProgressEmit)
                                .count();
        if (micros > 0) {
            rate = static_cast<double>(received - state.lastEmitReceived) *
                   1'000'000.0 / static_cast<double>(micros);
        }
    }
    state.lastEmitReceived = received;
    state.lastProgressEmit = now;
    return rate;
}

std::int64_t sumReceived(const DownloadEngine::DownloadState& state) {
    std::int64_t r = 0;
    for (const auto& t : state.tasks) r += t->bytesWritten();
    return r;
}

bool preallocateFile(const std::string& path, std::int64_t size, std::string* error) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        *error = std::string("open ") + path + ": " + std::strerror(errno);
        return false;
    }
    if (size > 0 && ::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        *error = std::string("ftruncate: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }
    ::close(fd);
    return true;
}

}  // namespace

// Pending retry slot stored on the engine (engine-thread-only access).
struct DownloadEngine::PendingRetry {
    CURL* easy;
    std::chrono::steady_clock::time_point dueAt;
};

// ---------- DownloadEngine ----------

DownloadEngine::DownloadEngine() {
    multi_ = curl_multi_init();
    if (!multi_) throw std::runtime_error("curl_multi_init failed");
    thread_ = std::thread([this] { runLoop(); });
}

DownloadEngine::~DownloadEngine() {
    stopRequested_.store(true, std::memory_order_release);
    if (multi_) curl_multi_wakeup(multi_);
    if (thread_.joinable()) thread_.join();
    // Anything still in flight at shutdown belongs to abandoned downloads --
    // free their resources so we don't leak open files / curl handles.
    for (auto* state : activeStates_) delete state;
    activeStates_.clear();
    if (multi_) curl_multi_cleanup(multi_);
}

void DownloadEngine::post(std::function<void()> cmd) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        commands_.push_back(std::move(cmd));
    }
    curl_multi_wakeup(multi_);
}

void DownloadEngine::addEasy(CURL* easy) {
    post([this, easy] { curl_multi_add_handle(multi_, easy); });
}

void DownloadEngine::probe(std::string url, std::function<void(ProbeResult)> onResult) {
    auto* state = new ProbeState();
    state->easy = curl_easy_init();
    state->onResult = std::move(onResult);
    state->url = std::move(url);

    // Probe by GETting a single-byte range. More robust than HEAD: works on
    // servers that drop HEADs entirely (e.g. some CDNs / nginx configs), and
    // a 206 response itself proves range support without needing
    // Accept-Ranges in the headers.
    curl_easy_setopt(state->easy, CURLOPT_URL, state->url.c_str());
    curl_easy_setopt(state->easy, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(state->easy, CURLOPT_WRITEFUNCTION, probeDiscardWrite);
    curl_easy_setopt(state->easy, CURLOPT_WRITEDATA, state);
    curl_easy_setopt(state->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(state->easy, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(state->easy, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(state->easy, CURLOPT_HEADERDATA, &state->header);
    curl_easy_setopt(state->easy, CURLOPT_USERAGENT, "fdm/0.1");
    // Without this, libcurl would deliver an HTML error page to our write
    // callback for 4xx/5xx, and our abort-on-non-206 logic would surface it as
    // CURLE_WRITE_ERROR -- masking the real cause (the HTTP status code).
    curl_easy_setopt(state->easy, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(state->easy, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(state->easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(state->easy, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(state->easy, CURLOPT_NOSIGNAL, 1L);

    state->ctx.onDone = [this, state](CURLcode rc) {
        long http = 0;
        curl_easy_getinfo(state->easy, CURLINFO_RESPONSE_CODE, &http);

        // CURLE_WRITE_ERROR is expected when we deliberately aborted a 200
        // full-body response inside probeDiscardWrite -- treat it as success
        // from the probe's perspective.
        const bool transportOk = (rc == CURLE_OK) || (rc == CURLE_WRITE_ERROR);

        const auto succeed = [&](bool supportsRanges, std::int64_t length) {
            ProbeResult result;
            result.ok = true;
            result.info.supportsRanges = supportsRanges;
            result.info.contentLength = length;
            char* eff = nullptr;
            curl_easy_getinfo(state->easy, CURLINFO_EFFECTIVE_URL, &eff);
            if (eff) result.info.finalUrl = eff;
            state->onResult(std::move(result));
            curl_easy_cleanup(state->easy);
            delete state;
        };

        if (transportOk && http == 206) {
            succeed(true, state->header.contentRangeTotal);
            return;
        }
        if (transportOk && http == 200) {
            succeed(state->header.acceptRanges, state->header.contentLength);
            return;
        }

        // Failure: retry transient errors with exponential backoff.
        if (state->attempts < kMaxProbeAttempts &&
            classifyError(rc, http) == RetryDecision::Retry) {
            state->attempts++;
            state->header = HeaderState{};
            retries_.push_back(PendingRetry{
                state->easy,
                std::chrono::steady_clock::now() + backoffFor(state->attempts - 1)});
            return;
        }

        ProbeResult result;
        result.error = describeError(rc, http);
        state->onResult(std::move(result));
        curl_easy_cleanup(state->easy);
        delete state;
    };
    curl_easy_setopt(state->easy, CURLOPT_PRIVATE, &state->ctx);

    addEasy(state->easy);
}

DownloadEngine::DownloadState* DownloadEngine::findById(DownloadId id) {
    for (auto* st : activeStates_) {
        if (st->id == id) return st;
    }
    return nullptr;
}

// Build ChunkTasks, emit Started, register for periodic Progress emits, and
// start each chunk on the multi handle. `restoreOverlay` carries per-chunk
// resume state (bytesReceived, attempts, status) when restoring from disk;
// nullptr means a fresh download. Caller already populated state->url,
// outputPath, onEvent, totalBytes, id.
void DownloadEngine::launchTasks(DownloadState* state,
                                 const std::vector<ChunkSpec>& chunks,
                                 const std::vector<ChunkRestore>* restoreOverlay) {
    if (chunks.empty()) {
        // Nothing to do (content-length 0). Emit a tidy Started + Finished
        // pair so callers can tear down without special-casing.
        state->emit(Started{state->totalBytes, state->supportsRanges, 0, {}});
        state->emit(Finished{});
        delete state;
        return;
    }

    state->totalChunks = static_cast<int>(chunks.size());
    state->chunkStatuses.assign(chunks.size(), ChunkProgress::Status::Active);

    // Pre-apply restore status so a chunk that finished in a previous session
    // doesn't kick off a new request, and the initial Started already shows
    // the right state to the UI.
    int alreadyDone = 0;
    if (restoreOverlay) {
        for (const auto& r : *restoreOverlay) {
            if (r.index >= 0 && r.index < state->totalChunks) {
                state->chunkStatuses[r.index] = r.status;
                if (r.status == ChunkProgress::Status::Done) alreadyDone++;
            }
        }
        state->completedChunks = alreadyDone;
    }

    state->emit(Started{state->totalBytes, state->supportsRanges,
                        state->totalChunks, chunks});

    try {
        for (const auto& spec : chunks) {
            std::int64_t initBytes = 0;
            int initAttempts = 1;
            if (restoreOverlay && spec.index < static_cast<int>(restoreOverlay->size())) {
                const auto& r = (*restoreOverlay)[spec.index];
                if (r.index == spec.index) {
                    initBytes = r.bytesReceived;
                    initAttempts = r.attempts;
                }
            }
            state->tasks.push_back(std::make_unique<ChunkTask>(
                state->url, state->outputPath, spec, initBytes, initAttempts));
        }
    } catch (const std::exception& e) {
        state->emit(Failed{e.what()});
        delete state;
        return;
    }

    // Register for periodic progress emits from runLoop. Initialise the
    // timer to "now" so the first emit fires ~kProgressInterval from here
    // rather than instantly (which would be a redundant 0% Progress).
    state->lastProgressEmit = std::chrono::steady_clock::now();
    state->lastEmitReceived = sumReceived(*state);
    activeStates_.push_back(state);

    // If every chunk was already Done in the restore overlay, the download
    // is already complete -- emit Finished and tear down without starting
    // any requests.
    if (state->completedChunks == state->totalChunks) {
        state->emit(Finished{});
        auto it = std::find(activeStates_.begin(), activeStates_.end(), state);
        if (it != activeStates_.end()) activeStates_.erase(it);
        delete state;
        return;
    }

    for (std::size_t i = 0; i < state->tasks.size(); ++i) {
        if (state->chunkStatuses[i] == ChunkProgress::Status::Done) continue;
        ChunkTask* taskPtr = state->tasks[i].get();
        taskPtr->start(*this, [this, state, taskPtr](CURLcode rc) {
            long http = 0;
            curl_easy_getinfo(taskPtr->easy(), CURLINFO_RESPONSE_CODE, &http);
            const int idx = taskPtr->spec().index;

            if (rc == CURLE_OK) {
                state->completedChunks++;
                state->chunkStatuses[idx] = ChunkProgress::Status::Done;
            } else if (classifyError(rc, http) == RetryDecision::Retry &&
                       taskPtr->prepareRetry()) {
                // Stays Status::Active -- the chunk is still in flight via
                // the retries_ queue; the UI sees this as "retrying" via
                // the attempts counter going up.
                const int retryAttempt = taskPtr->attempts() - 1;
                retries_.push_back(PendingRetry{
                    taskPtr->easy(),
                    std::chrono::steady_clock::now() + backoffFor(retryAttempt)});
                return;
            } else {
                state->failedChunks++;
                state->chunkStatuses[idx] = ChunkProgress::Status::Failed;
                if (state->firstError.empty()) {
                    state->firstError = describeError(rc, http);
                }
            }

            const auto now = std::chrono::steady_clock::now();
            const std::int64_t received = sumReceived(*state);

            if (state->completedChunks + state->failedChunks == state->totalChunks) {
                if (state->failedChunks == 0) {
                    const double rate = computeBytesPerSec(*state, received, now);
                    state->emit(Progress{received, state->totalBytes, rate,
                                         snapshotChunks(*state)});
                    state->emit(Finished{});
                } else {
                    state->emit(Failed{state->firstError});
                }
                auto it = std::find(activeStates_.begin(), activeStates_.end(), state);
                if (it != activeStates_.end()) activeStates_.erase(it);
                delete state;
            } else if (now - state->lastProgressEmit >= kProgressInterval) {
                const double rate = computeBytesPerSec(*state, received, now);
                state->emit(Progress{received, state->totalBytes, rate,
                                     snapshotChunks(*state)});
            }
        });
    }
}

DownloadId DownloadEngine::start(std::string url,
                                 std::string outputPath,
                                 std::function<void(EngineEvent)> onEvent) {
    auto* state = new DownloadState();
    state->id = nextId_.fetch_add(1, std::memory_order_relaxed);
    state->url = std::move(url);
    state->outputPath = std::move(outputPath);
    state->onEvent = std::move(onEvent);
    const DownloadId id = state->id;

    probe(state->url, [this, state](ProbeResult pr) {
        if (!pr.ok) {
            state->emit(Failed{pr.error});
            delete state;
            return;
        }

        state->totalBytes = pr.info.contentLength;
        state->supportsRanges = pr.info.supportsRanges;

        std::string err;
        if (!preallocateFile(state->outputPath, pr.info.contentLength, &err)) {
            state->emit(Failed{err});
            delete state;
            return;
        }

        std::vector<ChunkSpec> chunks;
        if (!pr.info.supportsRanges || pr.info.contentLength < 0) {
            chunks.push_back(ChunkSpec{0, -1, 0});
        } else {
            chunks = splitIntoChunks(pr.info.contentLength, kMaxChunks, kMinChunkSize);
        }
        launchTasks(state, chunks, /*restoreOverlay=*/nullptr);
    });
    return id;
}

DownloadId DownloadEngine::resumeKnown(ResumeSpec spec,
                                       std::function<void(EngineEvent)> onEvent) {
    auto* state = new DownloadState();
    state->id = nextId_.fetch_add(1, std::memory_order_relaxed);
    state->url = std::move(spec.url);
    state->outputPath = std::move(spec.outputPath);
    state->onEvent = std::move(onEvent);
    state->totalBytes = spec.totalBytes;
    state->supportsRanges = spec.supportsRanges;
    const DownloadId id = state->id;

    // Hop to the engine thread so libcurl / activeStates_ access is single-
    // threaded just like every other entry point.
    std::vector<ChunkRestore> restore = std::move(spec.chunks);
    post([this, state, restore = std::move(restore)]() mutable {
        // Build ChunkSpecs from the restore overlay so chunk-task indices stay
        // aligned. We trust the caller's split -- the partial file on disk was
        // sized for it.
        std::vector<ChunkSpec> chunks;
        chunks.reserve(restore.size());
        for (const auto& r : restore) {
            chunks.push_back(ChunkSpec{r.startByte, r.endByte, r.index});
        }
        launchTasks(state, chunks, &restore);
    });
    return id;
}

void DownloadEngine::runLoop() {
    while (!stopRequested_.load(std::memory_order_acquire)) {
        std::deque<std::function<void()>> local;
        {
            std::lock_guard<std::mutex> lk(mu_);
            local.swap(commands_);
        }
        for (auto& cmd : local) cmd();
        if (stopRequested_.load(std::memory_order_acquire)) break;

        // Re-add any pending retries whose delay has elapsed.
        const auto now = std::chrono::steady_clock::now();
        for (auto it = retries_.begin(); it != retries_.end();) {
            if (it->dueAt <= now) {
                curl_multi_add_handle(multi_, it->easy);
                it = retries_.erase(it);
            } else {
                ++it;
            }
        }

        int stillRunning = 0;
        curl_multi_perform(multi_, &stillRunning);

        int msgsLeft = 0;
        while (CURLMsg* msg = curl_multi_info_read(multi_, &msgsLeft)) {
            if (msg->msg != CURLMSG_DONE) continue;
            CURL* easy = msg->easy_handle;
            const CURLcode rc = msg->data.result;
            void* priv = nullptr;
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, &priv);
            curl_multi_remove_handle(multi_, easy);
            if (auto* ctx = static_cast<EasyContext*>(priv); ctx && ctx->onDone) {
                ctx->onDone(rc);
            }
        }

        // Periodic Progress for every active download. Independent of chunk
        // completion so the UI sees smooth byte-level updates instead of one
        // jump per chunk. Paused downloads are skipped -- their bytes aren't
        // advancing so a Progress would just look like a stalled stream.
        {
            const auto progNow = std::chrono::steady_clock::now();
            for (auto* st : activeStates_) {
                if (st->paused) continue;
                if (progNow - st->lastProgressEmit >= kProgressInterval) {
                    const std::int64_t r = sumReceived(*st);
                    const double rate = computeBytesPerSec(*st, r, progNow);
                    st->emit(Progress{r, st->totalBytes, rate, snapshotChunks(*st)});
                }
            }
        }

        // Don't sleep past the next scheduled retry, and cap at the progress
        // interval while a download is active so we wake often enough to keep
        // emitting at ~10 Hz.
        long pollMs = activeStates_.empty() ? 1000 : 100;
        for (const auto& r : retries_) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                r.dueAt - std::chrono::steady_clock::now())
                                .count();
            const long clamped = std::max<long>(0, static_cast<long>(ms));
            if (clamped < pollMs) pollMs = clamped;
        }
        curl_multi_poll(multi_, nullptr, 0, static_cast<int>(pollMs), nullptr);
    }
}

namespace {

// True if `r` belongs to one of `state`'s chunks (used when filtering the
// retry queue during pause/cancel).
bool retryBelongsTo(const DownloadEngine::PendingRetry& r,
                    const DownloadEngine::DownloadState& state) {
    for (const auto& t : state.tasks) {
        if (t->easy() == r.easy) return true;
    }
    return false;
}

}  // namespace

void DownloadEngine::pause(DownloadId id) {
    post([this, id] {
        auto* state = findById(id);
        if (!state || state->paused) return;
        for (auto& task : state->tasks) {
            // remove is a no-op for handles already off the multi (e.g.,
            // chunks that have completed) -- safe.
            curl_multi_remove_handle(multi_, task->easy());
        }
        retries_.erase(
            std::remove_if(retries_.begin(), retries_.end(),
                           [state](const PendingRetry& r) {
                               return retryBelongsTo(r, *state);
                           }),
            retries_.end());
        state->paused = true;
        state->emit(Paused{});
    });
}

void DownloadEngine::resume(DownloadId id) {
    post([this, id] {
        auto* state = findById(id);
        if (!state || !state->paused) return;
        for (std::size_t i = 0; i < state->tasks.size(); ++i) {
            const auto status = state->chunkStatuses[i];
            if (status != ChunkProgress::Status::Active) continue;
            auto& task = state->tasks[i];
            if (!task->reconfigureForResume()) {
                // Already fully received -- mark Done.
                state->chunkStatuses[i] = ChunkProgress::Status::Done;
                state->completedChunks++;
                continue;
            }
            curl_multi_add_handle(multi_, task->easy());
        }
        state->paused = false;
        // Reset the speed baseline so we don't show a huge spike from the
        // pause window.
        state->lastProgressEmit = std::chrono::steady_clock::now();
        state->lastEmitReceived = sumReceived(*state);
    });
}

void DownloadEngine::cancel(DownloadId id) {
    post([this, id] {
        auto* state = findById(id);
        if (!state) return;
        for (auto& task : state->tasks) {
            curl_multi_remove_handle(multi_, task->easy());
        }
        retries_.erase(
            std::remove_if(retries_.begin(), retries_.end(),
                           [state](const PendingRetry& r) {
                               return retryBelongsTo(r, *state);
                           }),
            retries_.end());
        state->emit(Failed{"Cancelled"});
        auto it = std::find(activeStates_.begin(), activeStates_.end(), state);
        if (it != activeStates_.end()) activeStates_.erase(it);
        delete state;
    });
}

}  // namespace fdm
