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

constexpr std::int64_t kMinChunkSize = 1024 * 1024;  // 1 MiB
constexpr std::chrono::milliseconds kProgressInterval{100};
constexpr int kMaxProbeAttempts = 4;  // total attempts including the first

// Adaptive concurrency: start at kInitialConnections, grow by 1 after sustained
// success (no throttle for kGrowQuietWindow), back off on 429, never exceed
// kMaxConnections. Dynamic re-segmentation won't split a remainder below
// kMinSplitBytes. A download with no byte progress for kStallTimeout fails.
constexpr int kInitialConnections = 4;
constexpr int kMaxConnections = 8;
constexpr std::int64_t kMinSplitBytes = 1024 * 1024;  // 1 MiB
constexpr std::chrono::seconds kStallTimeout{120};
constexpr std::chrono::seconds kGrowQuietWindow{5};

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
    std::string suggestedFilename;         // populated from Content-Disposition
    // Best digest captured so far. Subsequent stronger algorithms overwrite
    // weaker ones; weaker never overwrite stronger.
    std::string expectedHash;
    std::string hashAlgorithm;
    std::string hashSource;
};

// Strip path separators and control characters: a server cannot be trusted
// to send a clean leaf filename. (RFC 6266 says treat the value as a "bare
// filename" -- no path components.)
std::string sanitizeFilename(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '/' || c == '\\') continue;
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) continue;
        out += c;
    }
    return out;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

// Decode RFC 5987 ext-value: "charset'lang'percent-encoded-value".
// We treat the bytes as UTF-8 -- legal RFC 5987 charsets in practice are
// just UTF-8 (and the obsolete ISO-8859-1), and we hand the bytes to
// std::string callers either way. Returns empty on malformed input.
std::string decodeRfc5987(const std::string& raw) {
    const std::size_t firstTick = raw.find('\'');
    if (firstTick == std::string::npos) return {};
    const std::size_t secondTick = raw.find('\'', firstTick + 1);
    if (secondTick == std::string::npos) return {};
    const std::string encoded = raw.substr(secondTick + 1);

    std::string out;
    out.reserve(encoded.size());
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const char c = encoded[i];
        if (c == '%' && i + 2 < encoded.size()) {
            const int hi = hexValue(encoded[i + 1]);
            const int lo = hexValue(encoded[i + 2]);
            if (hi < 0 || lo < 0) return {};
            out += static_cast<char>((hi << 4) | lo);
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

// Read a Content-Disposition value and return the best filename found, or
// "" if none. Prefers filename*= (RFC 5987, non-ASCII-safe) over filename=.
// Tolerant: handles quoted and unquoted values, ignores unrecognized params,
// case-insensitive parameter names, RFC 2616 LWS around `=` and `;`.
std::string extractDispositionFilename(const char* p, const char* end) {
    std::string filename;
    std::string filenameStar;

    auto skipWs = [&]() { while (p < end && (*p == ' ' || *p == '\t')) ++p; };

    // Disposition type ("attachment", "inline", ...). Skip until ';' or EOL.
    while (p < end && *p != ';') ++p;

    while (p < end) {
        if (*p == ';') ++p;
        skipWs();
        // Parse parameter name (until '=').
        const char* nameStart = p;
        while (p < end && *p != '=' && *p != ';') ++p;
        if (p >= end || *p != '=') break;
        const char* nameEnd = p;
        // Trim trailing whitespace on name.
        while (nameEnd > nameStart && (*(nameEnd - 1) == ' ' || *(nameEnd - 1) == '\t')) --nameEnd;
        ++p;  // consume '='
        skipWs();

        // Parse parameter value: quoted-string or token.
        std::string value;
        if (p < end && *p == '"') {
            ++p;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) { value += *(p + 1); p += 2; }
                else { value += *p; ++p; }
            }
            if (p < end) ++p;  // consume closing quote
        } else {
            while (p < end && *p != ';' && *p != ' ' && *p != '\t') {
                value += *p;
                ++p;
            }
        }

        const std::size_t nameLen = nameEnd - nameStart;
        if (nameLen == 9 && strncasecmp(nameStart, "filename*", 9) == 0) {
            std::string decoded = decodeRfc5987(value);
            if (!decoded.empty()) filenameStar = std::move(decoded);
        } else if (nameLen == 8 && strncasecmp(nameStart, "filename", 8) == 0) {
            filename = std::move(value);
        }
        skipWs();
    }

    std::string raw = !filenameStar.empty() ? filenameStar : filename;
    return sanitizeFilename(std::move(raw));
}

// Strength order for digest selection. Higher = stronger.
int hashStrength(const std::string& alg) {
    if (alg == "sha256") return 3;
    if (alg == "sha1") return 2;
    if (alg == "md5") return 1;
    return 0;
}

// RFC 4648 base64 decode (no whitespace tolerance -- header values are
// expected to be clean tokens). Returns "" on malformed input.
std::string base64Decode(const std::string& s) {
    static const int kInvalid = -1;
    static int table[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) table[i] = kInvalid;
        const char* alpha =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(alpha[i])] = i;
        // base64url variants -- some senders use them.
        table[static_cast<unsigned char>('-')] = 62;
        table[static_cast<unsigned char>('_')] = 63;
        init = true;
    }

    std::string out;
    int buf = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=' || c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        const int v = table[static_cast<unsigned char>(c)];
        if (v == kInvalid) return {};
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    return out;
}

std::string toHexLower(const std::string& bytes) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out += d[c >> 4];
        out += d[c & 0xF];
    }
    return out;
}

bool isHexString(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

// Normalize an algorithm name. RFC 3230 used "MD5" / "SHA" / "SHA-256";
// RFC 9530 standardizes lowercase "sha-256" / "sha-1" / "md5"; in the wild
// you see all of these. Return canonical: "sha256" | "sha1" | "md5" | "".
std::string canonicalAlgorithm(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // Strip whitespace and dashes.
    std::string out;
    for (char c : s) if (c != '-' && c != ' ') out += c;
    if (out == "sha256") return "sha256";
    if (out == "sha1" || out == "sha") return "sha1";
    if (out == "md5") return "md5";
    return {};
}

std::size_t hexLengthFor(const std::string& alg) {
    if (alg == "sha256") return 64;
    if (alg == "sha1") return 40;
    if (alg == "md5") return 32;
    return 0;
}

// Decode a digest token's value into hex according to its encoding. Value
// is base64 in both RFC 3230 and 9530. Returns "" on malformed input or
// length mismatch with the declared algorithm.
std::string decodeDigestValue(const std::string& alg, const std::string& base64) {
    const std::string raw = base64Decode(base64);
    if (raw.empty()) return {};
    const std::size_t expected = hexLengthFor(alg) / 2;
    if (raw.size() != expected) return {};
    return toHexLower(raw);
}

// Updates st to use (alg, hexValue) iff `alg` is stronger than what's
// already stored. Empty hexValue is treated as "no update".
void considerDigest(HeaderState* st, const std::string& alg,
                    const std::string& hexValue, const std::string& source) {
    if (alg.empty() || hexValue.empty()) return;
    if (hashStrength(alg) <= hashStrength(st->hashAlgorithm)) return;
    st->expectedHash = hexValue;
    st->hashAlgorithm = alg;
    st->hashSource = source;
}

// Parse RFC 9530 "Content-Digest" value:
//   sha-256=:<base64>:, md5=:<base64>:
// Or RFC 3230 "Digest" value:
//   sha-256=<base64>, md5=<base64>
// Same comma-separated key=value structure; RFC 9530 wraps the value in
// colons (sf-binary). We strip the colons if present.
void parseDigestList(HeaderState* st, const char* p, const char* end,
                     const std::string& source) {
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == ',')) ++p;
        const char* algStart = p;
        while (p < end && *p != '=' && *p != ',') ++p;
        if (p >= end || *p != '=') break;
        std::string alg = canonicalAlgorithm(std::string(algStart, p - algStart));
        ++p;  // consume '='
        // Optional leading ':' (RFC 9530 sf-binary).
        if (p < end && *p == ':') ++p;
        const char* valStart = p;
        while (p < end && *p != ',' && *p != ':' && *p != ' ' && *p != '\t') ++p;
        std::string b64(valStart, p - valStart);
        // Optional trailing ':'.
        if (p < end && *p == ':') ++p;
        // Skip RFC 3230 quality qualifier (e.g. ";q=0.5") if present.
        while (p < end && *p != ',') ++p;
        considerDigest(st, alg, decodeDigestValue(alg, b64), source);
    }
}

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
    } else if (startsWithCI(buf, len, "content-disposition:")) {
        const char* p = skipValueWhitespace(buf + std::strlen("content-disposition:"), buf + len);
        std::string fn = extractDispositionFilename(p, buf + len);
        if (!fn.empty()) st->suggestedFilename = std::move(fn);
    } else if (startsWithCI(buf, len, "content-digest:")) {
        const char* p = skipValueWhitespace(buf + std::strlen("content-digest:"), buf + len);
        parseDigestList(st, p, buf + len, "content-digest");
    } else if (startsWithCI(buf, len, "digest:")) {
        const char* p = skipValueWhitespace(buf + std::strlen("digest:"), buf + len);
        parseDigestList(st, p, buf + len, "digest");
    } else if (startsWithCI(buf, len, "content-md5:")) {
        // Legacy: value is base64 of raw MD5 bytes, no algorithm prefix.
        const char* p = skipValueWhitespace(buf + std::strlen("content-md5:"), buf + len);
        std::string b64(p, buf + len - p);
        // Trim trailing whitespace.
        while (!b64.empty() && (b64.back() == ' ' || b64.back() == '\t')) b64.pop_back();
        considerDigest(st, "md5", decodeDigestValue("md5", b64), "content-md5");
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

// Build a curl_slist from raw "Name: value" header lines. Returns nullptr for
// an empty list. Caller owns the result and must curl_slist_free_all it once
// the easy handle no longer references it.
struct curl_slist* buildHeaderList(const std::vector<std::string>& headers) {
    struct curl_slist* list = nullptr;
    for (const std::string& h : headers) {
        if (h.empty()) continue;
        list = curl_slist_append(list, h.c_str());
    }
    return list;
}

struct ProbeState {
    HeaderState header;
    CURL* easy = nullptr;
    EasyContext ctx;
    std::function<void(ProbeResult)> onResult;
    int attempts = 1;
    std::string url;  // kept for diagnostic / re-issue purposes
    struct curl_slist* headerList = nullptr;  // freed at terminal cleanup
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

// One byte range within a download. Created at probe time and dynamically by
// re-segmentation. `task` is non-null only while Active; on pause / transient
// failure the task is moved to the state's reaping bin and the segment keeps
// its bytesReceived so it can be restarted from where it stopped.
struct DownloadEngine::Segment {
    int index = 0;
    std::int64_t startByte = 0;
    std::int64_t endByte = -1;        // inclusive; -1 only for the open-ended fallback
    std::int64_t bytesReceived = 0;   // snapshot when not Active (live count is in task)
    int attempts = 1;
    ChunkProgress::Status status = ChunkProgress::Status::Pending;
    std::unique_ptr<ChunkTask> task;  // owns the curl handle while Active
    std::chrono::steady_clock::time_point dueAt{};  // earliest (re)start when Pending
};

// One in-flight download. Created when start()/resumeKnown() is called; torn
// down by the run loop after a terminal event (done == true). Touched only from
// the engine thread.
struct DownloadEngine::DownloadState {
    DownloadId id = 0;
    std::string url;
    std::string outputPath;
    std::vector<std::string> headers;  // extra request headers, replayed per segment
    std::function<void(EngineEvent)> onEvent;
    std::int64_t totalBytes = -1;
    bool supportsRanges = false;
    bool openEnded = false;            // unknown size / no ranges -> single EOF-terminated segment
    std::string expectedHash;
    std::string hashAlgorithm;
    std::string hashSource;

    std::deque<Segment> segments;      // deque: element pointers survive push_back
    int nextSegmentIndex = 0;
    int connLimit = kInitialConnections;
    int activeCount = 0;
    std::chrono::steady_clock::time_point lastThrottle{};

    // Tasks awaiting destruction -- a ChunkTask can't be deleted from inside its
    // own curl completion callback; cleared at the top of each run-loop tick.
    std::vector<std::unique_ptr<ChunkTask>> reaping;

    bool paused = false;
    bool done = false;                 // terminal emitted; reap next tick
    std::string fatalError;

    // Rate + stall tracking.
    std::chrono::steady_clock::time_point lastProgressEmit{};
    std::int64_t lastEmitReceived = 0;
    std::chrono::steady_clock::time_point lastAdvanceAt{};
    std::int64_t lastAdvanceBytes = 0;

    void emit(EngineEvent ev) {
        if (onEvent) onEvent(std::move(ev));
    }
};

namespace {

// Build the per-segment snapshot included in every Progress event. Active
// segments report their live byte count from the task; others use the snapshot.
std::vector<ChunkProgress> snapshotChunks(const DownloadEngine::DownloadState& state) {
    std::vector<ChunkProgress> out;
    out.reserve(state.segments.size());
    for (const auto& seg : state.segments) {
        ChunkProgress cp;
        cp.index = seg.index;
        cp.startByte = seg.startByte;
        cp.endByte = seg.endByte;
        cp.bytesReceived = seg.task ? seg.task->bytesWritten() : seg.bytesReceived;
        cp.attempts = seg.attempts;
        cp.status = seg.status;
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
    for (const auto& seg : state.segments)
        r += seg.task ? seg.task->bytesWritten() : seg.bytesReceived;
    return r;
}

bool preallocateFile(const std::string& path, std::int64_t size, std::string* error) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        *error = std::string("open ") + path + ": " + std::strerror(errno);
        return false;
    }
    if (size > 0) {
        // posix_fallocate reserves real blocks (vs. ftruncate's sparse hole),
        // avoiding fragmentation and surfacing ENOSPC now rather than mid-write.
        // It returns an errno-style code (not via errno) and 0 on success.
        const int rc = ::posix_fallocate(fd, 0, static_cast<off_t>(size));
        if (rc == ENOSPC) {
            *error = std::string("posix_fallocate: ") + std::strerror(rc);
            ::close(fd);
            return false;
        }
        // Filesystem may not support fallocate -- fall back to a sparse file.
        if (rc != 0 && ::ftruncate(fd, static_cast<off_t>(size)) != 0) {
            *error = std::string("ftruncate: ") + std::strerror(errno);
            ::close(fd);
            return false;
        }
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
    // Reuse connections, DNS results, and TLS sessions across every handle.
    // All handles run on the engine thread, so no lock callbacks are needed.
    share_ = curl_share_init();
    if (share_) {
        curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
        curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    }
    // Let chunk requests to an HTTP/2 host multiplex over a single connection
    // instead of opening (and handshaking) a fresh one each. No-op on HTTP/1.x.
    curl_multi_setopt(multi_, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
    thread_ = std::thread([this] { runLoop(); });
}

DownloadEngine::~DownloadEngine() {
    stopRequested_.store(true, std::memory_order_release);
    if (multi_) curl_multi_wakeup(multi_);
    if (thread_.joinable()) thread_.join();
    // Anything still in flight at shutdown belongs to abandoned downloads --
    // free their resources so we don't leak open files / curl handles. Remove
    // each still-active handle from the multi before its task is destroyed
    // (curl forbids cleaning up an easy handle still attached to a multi).
    for (auto* state : activeStates_) {
        for (auto& seg : state->segments)
            if (seg.task) curl_multi_remove_handle(multi_, seg.task->easy());
        delete state;
    }
    activeStates_.clear();
    if (multi_) curl_multi_cleanup(multi_);
    // Cleaned up only after every easy handle that referenced it is gone.
    if (share_) curl_share_cleanup(share_);
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
    probe(std::move(url), {}, std::move(onResult));
}

void DownloadEngine::probe(std::string url, std::vector<std::string> headers,
                           std::function<void(ProbeResult)> onResult) {
    auto* state = new ProbeState();
    state->easy = curl_easy_init();
    state->onResult = std::move(onResult);
    state->url = std::move(url);
    state->headerList = buildHeaderList(headers);

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
    // Caller-supplied headers (Cookie / Referer / a real browser User-Agent).
    // A User-Agent here overrides the default set just above.
    if (state->headerList) {
        curl_easy_setopt(state->easy, CURLOPT_HTTPHEADER, state->headerList);
    }
    // Without this, libcurl would deliver an HTML error page to our write
    // callback for 4xx/5xx, and our abort-on-non-206 logic would surface it as
    // CURLE_WRITE_ERROR -- masking the real cause (the HTTP status code).
    curl_easy_setopt(state->easy, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(state->easy, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(state->easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(state->easy, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(state->easy, CURLOPT_NOSIGNAL, 1L);
    // Warm the shared TLS-session / connection cache so the chunk requests that
    // follow can resume this probe's session instead of re-handshaking.
    if (share_) curl_easy_setopt(state->easy, CURLOPT_SHARE, share_);

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
            result.info.suggestedFilename = state->header.suggestedFilename;
            result.info.expectedHash = state->header.expectedHash;
            result.info.hashAlgorithm = state->header.hashAlgorithm;
            result.info.hashSource = state->header.hashSource;
            char* eff = nullptr;
            curl_easy_getinfo(state->easy, CURLINFO_EFFECTIVE_URL, &eff);
            if (eff) result.info.finalUrl = eff;
            state->onResult(std::move(result));
            curl_easy_cleanup(state->easy);
            if (state->headerList) curl_slist_free_all(state->headerList);
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
        if (state->headerList) curl_slist_free_all(state->headerList);
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

DownloadEngine::Segment* DownloadEngine::findSegment(DownloadState* state, int index) {
    for (auto& seg : state->segments)
        if (seg.index == index) return &seg;
    return nullptr;
}

// Seed a download's initial segments, emit Started (fresh only), register it,
// and start pumping. `restoreOverlay` carries per-segment resume state (nullptr
// for a brand-new download). Caller already populated url/outputPath/onEvent/
// totalBytes/openEnded/id.
void DownloadEngine::beginDownload(DownloadState* state,
                                   const std::vector<ChunkSpec>& chunks,
                                   const std::vector<ChunkRestore>* restoreOverlay) {
    if (chunks.empty()) {
        // Nothing to do (content-length 0). Emit a tidy Started + Finished pair.
        Started zero;
        zero.contentLength = state->totalBytes;
        zero.supportsRanges = state->supportsRanges;
        zero.chunkCount = 0;
        zero.expectedHash = state->expectedHash;
        zero.hashAlgorithm = state->hashAlgorithm;
        zero.hashSource = state->hashSource;
        state->emit(std::move(zero));
        state->emit(Finished{});
        delete state;
        return;
    }

    int maxIndex = -1;
    for (const ChunkSpec& spec : chunks) {
        Segment seg;
        seg.index = spec.index;
        seg.startByte = spec.startByte;
        seg.endByte = spec.endByte;
        seg.status = ChunkProgress::Status::Pending;
        if (restoreOverlay) {
            for (const auto& r : *restoreOverlay) {
                if (r.index == spec.index) {
                    seg.bytesReceived = r.bytesReceived;
                    seg.attempts = r.attempts;
                    if (r.status == ChunkProgress::Status::Done)
                        seg.status = ChunkProgress::Status::Done;
                    break;
                }
            }
        }
        maxIndex = std::max(maxIndex, seg.index);
        state->segments.push_back(std::move(seg));
    }
    state->nextSegmentIndex = maxIndex + 1;

    // Fresh downloads announce their initial layout. Resumes already have the
    // layout persisted and let Progress events refresh it -- emitting Started on
    // resume would briefly zero the on-disk progress in the DB.
    if (!restoreOverlay) {
        Started started;
        started.contentLength = state->totalBytes;
        started.supportsRanges = state->supportsRanges;
        started.chunkCount = static_cast<int>(chunks.size());
        started.chunks = chunks;
        started.expectedHash = state->expectedHash;
        started.hashAlgorithm = state->hashAlgorithm;
        started.hashSource = state->hashSource;
        state->emit(std::move(started));
    }

    const auto now = std::chrono::steady_clock::now();
    state->lastProgressEmit = now;
    state->lastEmitReceived = sumReceived(*state);
    state->lastAdvanceAt = now;
    state->lastAdvanceBytes = state->lastEmitReceived;
    activeStates_.push_back(state);

    bool allDone = true;
    for (const auto& seg : state->segments)
        if (seg.status != ChunkProgress::Status::Done) { allDone = false; break; }
    if (allDone) {
        state->emit(Finished{});
        state->done = true;  // reaped on the next run-loop tick
        return;
    }
    pump(state);
}

// Fill free connection slots: start Pending segments whose backoff has elapsed,
// and split the largest in-flight segment when nothing else is pending.
void DownloadEngine::pump(DownloadState* state) {
    if (state->done || state->paused) return;
    const auto now = std::chrono::steady_clock::now();
    while (state->activeCount < state->connLimit) {
        Segment* seg = nullptr;
        for (auto& s : state->segments) {
            if (s.status == ChunkProgress::Status::Pending && s.dueAt <= now) {
                seg = &s;
                break;
            }
        }
        if (!seg) seg = trySplitLargestActive(state);
        if (!seg) break;
        startSegment(state, seg);
        if (state->done) break;  // startSegment hit a fatal error
    }
}

void DownloadEngine::startSegment(DownloadState* state, Segment* seg) {
    try {
        seg->task = std::make_unique<ChunkTask>(
            state->url, state->outputPath,
            ChunkSpec{seg->startByte, seg->endByte, seg->index}, state->headers,
            seg->bytesReceived, seg->attempts);
    } catch (const std::exception& e) {
        failDownload(state, e.what());  // couldn't open the file etc.
        return;
    }
    seg->status = ChunkProgress::Status::Active;
    state->activeCount++;

    const DownloadId id = state->id;
    const int segIndex = seg->index;
    seg->task->start(*this, [this, id, segIndex](CURLcode rc) {
        DownloadState* st = findById(id);
        if (!st) return;
        Segment* s = findSegment(st, segIndex);
        if (!s || !s->task) return;
        long http = 0;
        curl_easy_getinfo(s->task->easy(), CURLINFO_RESPONSE_CODE, &http);
        const bool capped = s->task->cappedComplete();
        onSegmentDone(st, segIndex, rc, http, capped);
    });
}

// Dynamic re-segmentation: take the Active segment with the most unfetched
// bytes, cap it at the midpoint of its remainder, and return a new Pending
// segment covering the tail. Returns nullptr if nothing is worth splitting.
DownloadEngine::Segment* DownloadEngine::trySplitLargestActive(DownloadState* state) {
    Segment* best = nullptr;
    std::int64_t bestRemaining = 0;
    for (auto& seg : state->segments) {
        if (seg.status != ChunkProgress::Status::Active || !seg.task) continue;
        if (seg.endByte < 0) continue;  // open-ended: can't split
        const std::int64_t pos = seg.startByte + seg.task->bytesWritten();
        const std::int64_t remaining = seg.endByte - pos + 1;
        if (remaining > bestRemaining) {
            bestRemaining = remaining;
            best = &seg;
        }
    }
    if (!best || bestRemaining / 2 < kMinSplitBytes) return nullptr;

    const std::int64_t pos = best->startByte + best->task->bytesWritten();
    const std::int64_t mid = pos + bestRemaining / 2 - 1;  // best keeps [start, mid]
    Segment tail;
    tail.index = state->nextSegmentIndex++;
    tail.startByte = mid + 1;
    tail.endByte = best->endByte;
    tail.status = ChunkProgress::Status::Pending;

    best->endByte = mid;
    best->task->capEndAt(mid);  // the running connection now stops at the midpoint

    state->segments.push_back(std::move(tail));
    return &state->segments.back();
}

// Per-segment terminal handler, invoked from the curl completion callback.
void DownloadEngine::onSegmentDone(DownloadState* state, int segIndex, CURLcode rc,
                                   long http, bool capped) {
    Segment* seg = findSegment(state, segIndex);
    if (!seg) return;

    // Snapshot received bytes and retire the task (destroyed at the next tick --
    // we're inside its own callback right now).
    if (seg->task) {
        seg->bytesReceived = seg->task->bytesWritten();
        state->reaping.push_back(std::move(seg->task));
    }
    state->activeCount--;
    if (state->done) return;  // a sibling already drove us terminal

    if (capped || rc == CURLE_OK) {
        seg->status = ChunkProgress::Status::Done;
        // Adaptive grow: after a quiet spell with no throttling, allow one more
        // connection (up to the cap). pump() opens it by splitting.
        const auto now = std::chrono::steady_clock::now();
        if (state->connLimit < kMaxConnections &&
            (state->lastThrottle.time_since_epoch().count() == 0 ||
             now - state->lastThrottle >= kGrowQuietWindow)) {
            state->connLimit++;
        }
    } else if (classifyError(rc, http) == RetryDecision::Retry) {
        // Transient (429 / connect / timeout / reset): requeue -- never fatal.
        seg->status = ChunkProgress::Status::Pending;
        seg->attempts++;
        const auto now = std::chrono::steady_clock::now();
        seg->dueAt = now + backoffFor(seg->attempts - 1);
        if (http == 429) {
            // Server is rate-limiting concurrency: back the cap off (AIMD).
            state->connLimit = std::max(1, state->connLimit - 1);
            state->lastThrottle = now;
        }
    } else {
        failDownload(state, describeError(rc, http));  // 403/404/TLS/... -> fatal
        return;
    }

    maybeFinish(state);
    if (!state->done) pump(state);
}

void DownloadEngine::maybeFinish(DownloadState* state) {
    for (const auto& seg : state->segments)
        if (seg.status != ChunkProgress::Status::Done) return;
    // All segments Done -> the file is complete (segments tile [0,total)).
    const auto now = std::chrono::steady_clock::now();
    const std::int64_t received = sumReceived(*state);
    const double rate = computeBytesPerSec(*state, received, now);
    state->emit(Progress{received, state->totalBytes, rate, snapshotChunks(*state),
                         state->activeCount, state->connLimit});
    state->emit(Finished{});
    state->done = true;
}

void DownloadEngine::failDownload(DownloadState* state, const std::string& reason) {
    if (state->done) return;
    for (auto& seg : state->segments) {
        if (seg.task) {
            // Remove the handle before its task is destroyed next tick.
            curl_multi_remove_handle(multi_, seg.task->easy());
            state->reaping.push_back(std::move(seg.task));
        }
        if (seg.status == ChunkProgress::Status::Active ||
            seg.status == ChunkProgress::Status::Pending)
            seg.status = ChunkProgress::Status::Failed;
    }
    state->activeCount = 0;
    state->fatalError = reason;
    state->emit(Failed{reason});
    state->done = true;
}

DownloadId DownloadEngine::start(std::string url,
                                 std::string outputPath,
                                 std::function<void(EngineEvent)> onEvent) {
    return start(std::move(url), std::move(outputPath), {}, std::move(onEvent));
}

DownloadId DownloadEngine::start(std::string url,
                                 std::string outputPath,
                                 std::vector<std::string> headers,
                                 std::function<void(EngineEvent)> onEvent) {
    auto* state = new DownloadState();
    state->id = nextId_.fetch_add(1, std::memory_order_relaxed);
    state->url = std::move(url);
    state->outputPath = std::move(outputPath);
    state->headers = std::move(headers);
    state->onEvent = std::move(onEvent);
    const DownloadId id = state->id;

    probe(state->url, state->headers, [this, state](ProbeResult pr) {
        if (!pr.ok) {
            state->emit(Failed{pr.error});
            delete state;
            return;
        }

        // Download the chunks straight from the post-redirect URL so each
        // connection (and every retry) skips re-walking the redirect chain.
        // FOLLOWLOCATION stays on per chunk as a safety net if it redirects
        // again; an expired signed URL surfaces as a normal HTTP failure.
        if (!pr.info.finalUrl.empty()) state->url = pr.info.finalUrl;

        state->totalBytes = pr.info.contentLength;
        state->supportsRanges = pr.info.supportsRanges;
        state->expectedHash = pr.info.expectedHash;
        state->hashAlgorithm = pr.info.hashAlgorithm;
        state->hashSource = pr.info.hashSource;

        std::string err;
        if (!preallocateFile(state->outputPath, pr.info.contentLength, &err)) {
            state->emit(Failed{err});
            delete state;
            return;
        }

        std::vector<ChunkSpec> chunks;
        if (!pr.info.supportsRanges || pr.info.contentLength < 0) {
            state->openEnded = true;
            chunks.push_back(ChunkSpec{0, -1, 0});
        } else {
            // Seed only the initial connections; the pool grows segments
            // dynamically as connections free up (see trySplitLargestActive).
            chunks = splitIntoChunks(pr.info.contentLength, kInitialConnections,
                                     kMinChunkSize);
        }
        beginDownload(state, chunks, /*restoreOverlay=*/nullptr);
    });
    return id;
}

DownloadId DownloadEngine::resumeKnown(ResumeSpec spec,
                                       std::function<void(EngineEvent)> onEvent) {
    auto* state = new DownloadState();
    state->id = nextId_.fetch_add(1, std::memory_order_relaxed);
    state->url = std::move(spec.url);
    state->outputPath = std::move(spec.outputPath);
    state->headers = std::move(spec.headers);
    state->onEvent = std::move(onEvent);
    state->totalBytes = spec.totalBytes;
    state->supportsRanges = spec.supportsRanges;
    state->openEnded = (!spec.supportsRanges || spec.totalBytes < 0);
    const DownloadId id = state->id;

    // Hop to the engine thread so libcurl / activeStates_ access is single-
    // threaded just like every other entry point.
    std::vector<ChunkRestore> restore = std::move(spec.chunks);
    post([this, state, restore = std::move(restore)]() mutable {
        // Build ChunkSpecs from the restore overlay so segment indices stay
        // aligned. We trust the caller's split -- the partial file on disk was
        // sized for it. The scheduler resumes dynamic re-segmentation from here.
        std::vector<ChunkSpec> chunks;
        chunks.reserve(restore.size());
        for (const auto& r : restore) {
            chunks.push_back(ChunkSpec{r.startByte, r.endByte, r.index});
        }
        beginDownload(state, chunks, &restore);
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

        const auto now = std::chrono::steady_clock::now();

        // Reap finished ChunkTasks (frees curl handles) and tear down completed
        // downloads. Done here -- outside any curl callback -- so destroying
        // tasks / states is safe.
        for (auto it = activeStates_.begin(); it != activeStates_.end();) {
            DownloadState* st = *it;
            st->reaping.clear();
            if (st->done) {
                delete st;
                it = activeStates_.erase(it);
            } else {
                ++it;
            }
        }

        // Probe retries (segment retries are driven by the scheduler, not here).
        for (auto it = retries_.begin(); it != retries_.end();) {
            if (it->dueAt <= now) {
                curl_multi_add_handle(multi_, it->easy);
                it = retries_.erase(it);
            } else {
                ++it;
            }
        }

        // Start pending segments whose backoff has elapsed and open grown slots.
        for (auto* st : activeStates_) {
            if (!st->paused && !st->done) pump(st);
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

        // Periodic Progress + stall watchdog for every live download.
        {
            const auto progNow = std::chrono::steady_clock::now();
            for (auto* st : activeStates_) {
                if (st->paused || st->done) continue;
                const std::int64_t r = sumReceived(*st);
                if (r > st->lastAdvanceBytes) {
                    st->lastAdvanceBytes = r;
                    st->lastAdvanceAt = progNow;
                } else if (st->activeCount == 0 &&
                           progNow - st->lastAdvanceAt >= kStallTimeout) {
                    // Nothing in flight and no progress for a long time -- give
                    // up rather than spin on a server that keeps refusing.
                    failDownload(st, "stalled (no progress)");
                    continue;
                }
                if (progNow - st->lastProgressEmit >= kProgressInterval) {
                    const double rate = computeBytesPerSec(*st, r, progNow);
                    st->emit(Progress{r, st->totalBytes, rate, snapshotChunks(*st),
                                      st->activeCount, st->connLimit});
                }
            }
        }

        // Wake in time for the next pending-segment backoff / probe retry, and
        // at least every 100ms while a download is active.
        long pollMs = activeStates_.empty() ? 1000 : 100;
        const auto nowPoll = std::chrono::steady_clock::now();
        auto consider = [&](std::chrono::steady_clock::time_point t) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                t - nowPoll)
                                .count();
            const long clamped = std::max<long>(0, static_cast<long>(ms));
            if (clamped < pollMs) pollMs = clamped;
        };
        for (const auto& r : retries_) consider(r.dueAt);
        for (auto* st : activeStates_) {
            if (st->paused || st->done) continue;
            for (const auto& seg : st->segments)
                if (seg.status == ChunkProgress::Status::Pending) consider(seg.dueAt);
        }
        curl_multi_poll(multi_, nullptr, 0, static_cast<int>(pollMs), nullptr);
    }
}

void DownloadEngine::pause(DownloadId id) {
    post([this, id] {
        auto* state = findById(id);
        if (!state || state->paused || state->done) return;
        for (auto& seg : state->segments) {
            if (seg.task) {
                curl_multi_remove_handle(multi_, seg.task->easy());
                seg.bytesReceived = seg.task->bytesWritten();
                state->reaping.push_back(std::move(seg.task));
            }
            if (seg.status == ChunkProgress::Status::Active)
                seg.status = ChunkProgress::Status::Pending;
            seg.dueAt = {};  // immediately eligible when resumed
        }
        state->activeCount = 0;
        state->paused = true;
        state->emit(Paused{});
    });
}

void DownloadEngine::resume(DownloadId id) {
    post([this, id] {
        auto* state = findById(id);
        if (!state || !state->paused || state->done) return;
        state->paused = false;
        // Reset the speed + stall baselines so the pause window doesn't show as
        // a spike or trip the watchdog.
        const auto now = std::chrono::steady_clock::now();
        state->lastProgressEmit = now;
        state->lastEmitReceived = sumReceived(*state);
        state->lastAdvanceAt = now;
        state->lastAdvanceBytes = state->lastEmitReceived;
        pump(state);
    });
}

void DownloadEngine::cancel(DownloadId id) {
    post([this, id] {
        auto* state = findById(id);
        if (!state || state->done) return;
        for (auto& seg : state->segments) {
            if (seg.task) {
                curl_multi_remove_handle(multi_, seg.task->easy());
                state->reaping.push_back(std::move(seg.task));
            }
        }
        state->activeCount = 0;
        state->emit(Failed{"Cancelled"});
        state->done = true;  // reaped on the next run-loop tick
    });
}

}  // namespace fdm
