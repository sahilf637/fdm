#include "ChunkTask.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <strings.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace fdm {

ChunkTask::ChunkTask(std::string url, std::string outputPath, ChunkSpec spec,
                     std::vector<std::string> headers,
                     std::int64_t initialBytesReceived, int initialAttempts,
                     const std::string& validator)
    : url_(std::move(url)),
      outputPath_(std::move(outputPath)),
      spec_(spec),
      bytesWrittenSoFar_(initialBytesReceived),
      attempts_(initialAttempts < 1 ? 1 : initialAttempts) {
    // Effective end starts at the segment's declared end; dynamic
    // re-segmentation may lower it later via capEndAt().
    capEnd_ = spec_.endByte;
    // pwrite addresses each write by absolute offset, so there's no shared file
    // position to seek and sibling chunks can write disjoint regions of the
    // same file concurrently. The offset is recomputed per write callback.
    fd_ = ::open(outputPath_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        throw std::runtime_error(std::string("open ") + outputPath_ + ": " +
                                 std::strerror(errno));
    }

    easy_ = curl_easy_init();
    if (!easy_) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("curl_easy_init failed");
    }

    curl_easy_setopt(easy_, CURLOPT_URL, url_.c_str());
    bool sendsRange = false;
    if (spec_.endByte >= 0) {
        // Resume past any bytes already on disk from a previous run.
        const std::int64_t writeOffset = spec_.startByte + bytesWrittenSoFar_;
        const std::string range = std::to_string(writeOffset) + "-" +
                                  std::to_string(spec_.endByte);
        curl_easy_setopt(easy_, CURLOPT_RANGE, range.c_str());
        sendsRange = true;
    } else if (bytesWrittenSoFar_ > 0) {
        // Open-ended resume (pause/retry/cross-restart of a download whose
        // server didn't advertise ranges): still ask for the tail. If the
        // server honors it (206) we continue where we stopped; if it ignores
        // it (200, full body) the write callback restarts from offset 0 --
        // without the Range request we'd write the body's first byte at
        // offset bytesWrittenSoFar_ and corrupt the file.
        const std::string range =
            std::to_string(spec_.startByte + bytesWrittenSoFar_) + "-";
        curl_easy_setopt(easy_, CURLOPT_RANGE, range.c_str());
        openEndedResume_ = true;
        sendsRange = true;
    }
    curl_easy_setopt(easy_, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy_, CURLOPT_MAXREDIRS, 5L);
    // A malicious/misconfigured redirect must not bounce us to FTP, file://
    // etc. (libcurl's default redirect set still includes FTP).
#if CURL_AT_LEAST_VERSION(7, 85, 0)
    curl_easy_setopt(easy_, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy_, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(easy_, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(easy_, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(easy_, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(easy_, CURLOPT_WRITEFUNCTION, &ChunkTask::writeCallback);
    curl_easy_setopt(easy_, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(easy_, CURLOPT_HEADERFUNCTION, &ChunkTask::headerCallback);
    curl_easy_setopt(easy_, CURLOPT_HEADERDATA, this);
    curl_easy_setopt(easy_, CURLOPT_USERAGENT, "fdm/0.1");
    // Forward caller-supplied headers (Cookie / Referer / a real browser
    // User-Agent). A User-Agent here overrides the default set just above.
    for (const std::string& h : headers) {
        if (!h.empty()) headers_ = curl_slist_append(headers_, h.c_str());
    }
    // Tie the ranged request to the probed version of the resource: if it
    // changed, a conditional server answers 200 (full body) instead of 206,
    // which the write callback rejects -- failing loudly instead of writing
    // bytes from a different file version.
    if (sendsRange && !validator.empty()) {
        const std::string ifRange = "If-Range: " + validator;
        headers_ = curl_slist_append(headers_, ifRange.c_str());
    }
    if (headers_) curl_easy_setopt(easy_, CURLOPT_HTTPHEADER, headers_);
    // Bound how long we'll wait for a dead connection before bailing -- a
    // stalled chunk should fail and trigger retry rather than hang forever.
    curl_easy_setopt(easy_, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(easy_, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(easy_, CURLOPT_LOW_SPEED_TIME, 30L);
    // Allow libcurl's connection pool to terminate this transfer cleanly
    // without leaking sockets if we abort.
    curl_easy_setopt(easy_, CURLOPT_NOSIGNAL, 1L);
    // Larger receive buffer -> fewer write-callback/syscall round trips on fast
    // links; keepalive holds idle connections open for reuse on long transfers.
    curl_easy_setopt(easy_, CURLOPT_BUFFERSIZE, 262144L);
    curl_easy_setopt(easy_, CURLOPT_TCP_KEEPALIVE, 1L);
}

ChunkTask::~ChunkTask() {
    if (easy_) curl_easy_cleanup(easy_);
    if (headers_) curl_slist_free_all(headers_);
    if (fd_ >= 0) ::close(fd_);
}

void ChunkTask::start(DownloadEngine& engine, std::function<void(CURLcode)> onComplete) {
    // pwrite lands bytes straight in the page cache, so a downstream reader sees
    // them immediately -- no buffer flush needed before notifying the caller.
    ctx_.onDone = std::move(onComplete);
    curl_easy_setopt(easy_, CURLOPT_PRIVATE, &ctx_);
    // Reuse the engine's shared DNS / TLS-session / connection cache.
    if (CURLSH* sh = engine.shareHandle()) curl_easy_setopt(easy_, CURLOPT_SHARE, sh);
    engine.addEasy(easy_);
}

std::size_t ChunkTask::headerCallback(char* buf, std::size_t size, std::size_t nmemb,
                                      void* userp) {
    auto* self = static_cast<ChunkTask*>(userp);
    const std::size_t total = size * nmemb;

    std::size_t len = total;
    while (len && (buf[len - 1] == '\r' || buf[len - 1] == '\n')) --len;

    // A new status line means a new response (redirect hop / retry-after-301):
    // forget values parsed from the previous response.
    if (len >= 5 && strncasecmp(buf, "HTTP/", 5) == 0) {
        self->contentRangeStart_ = -1;
        self->retryAfterSecs_ = -1;
        return total;
    }

    auto valueAfter = [&](const char* name) -> const char* {
        const std::size_t nlen = std::strlen(name);
        if (len < nlen || strncasecmp(buf, name, nlen) != 0) return nullptr;
        const char* p = buf + nlen;
        const char* end = buf + len;
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        return p;
    };

    if (const char* p = valueAfter("content-range:")) {
        // "bytes A-B/T" -- capture A so the first write can verify the server
        // honored the offset we asked for.
        const char* end = buf + len;
        if (end - p >= 5 && strncasecmp(p, "bytes", 5) == 0) {
            p += 5;
            while (p < end && (*p == ' ' || *p == '\t')) ++p;
            if (p < end && *p >= '0' && *p <= '9') {
                self->contentRangeStart_ = std::strtoll(p, nullptr, 10);
            }
        }
    } else if (const char* p2 = valueAfter("retry-after:")) {
        // Seconds form only; the HTTP-date form is rare and safely ignored
        // (the engine falls back to its normal exponential backoff).
        if (p2 < buf + len && *p2 >= '0' && *p2 <= '9') {
            self->retryAfterSecs_ = std::strtol(p2, nullptr, 10);
        }
    }
    return total;
}

std::size_t ChunkTask::writeCallback(char* ptr, std::size_t size, std::size_t nmemb,
                                     void* userp) {
    auto* self = static_cast<ChunkTask*>(userp);
    const std::size_t total = size * nmemb;

    // Defense against servers that lie about range support: if we asked for a
    // Range but the server returned 200 (full body) instead of 206, writing
    // here would corrupt the file at this chunk's offset. Abort by returning
    // a short write -> libcurl raises CURLE_WRITE_ERROR -> the engine treats
    // it as a chunk failure. (With If-Range set, this is also how a changed
    // resource manifests.)
    if (self->spec_.endByte >= 0) {
        long http = 0;
        curl_easy_getinfo(self->easy_, CURLINFO_RESPONSE_CODE, &http);
        if (http == 200) {
            self->sawFullBodyForRange_ = true;
            return 0;
        }
        // Broken server/proxy answering 206 with a different range than
        // requested: writing it at our offset would corrupt the file.
        if (!self->rangeChecked_) {
            self->rangeChecked_ = true;
            const std::int64_t asked = self->spec_.startByte + self->bytesWrittenSoFar_;
            if (self->contentRangeStart_ >= 0 && self->contentRangeStart_ != asked) {
                self->sawFullBodyForRange_ = true;  // same fatal "wrong bytes" class
                return 0;
            }
        }
    }

    // First write of an open-ended resume: a 200 means the server ignored our
    // Range request and is sending the full body, so restart writing from the
    // segment's start instead of appending the body after the resumed bytes.
    if (self->openEndedResume_) {
        self->openEndedResume_ = false;
        long http = 0;
        curl_easy_getinfo(self->easy_, CURLINFO_RESPONSE_CODE, &http);
        if (http == 200) self->bytesWrittenSoFar_ = 0;
    }

    // pwrite targets an absolute offset (chunk start + bytes already written),
    // so the shared file needs no per-task seek and sibling chunks never race
    // on a file position.
    const std::int64_t pos = self->spec_.startByte + self->bytesWrittenSoFar_;
    const off_t offset = static_cast<off_t>(pos);

    // Open-ended segment (unknown size / no ranges): write everything.
    if (self->capEnd_ < 0) {
        const ssize_t w = ::pwrite(self->fd_, ptr, total, offset);
        if (w <= 0) return 0;  // disk error -> WRITE_ERROR
        self->bytesWrittenSoFar_ += static_cast<std::int64_t>(w);
        return static_cast<std::size_t>(w);  // short write -> WRITE_ERROR (disk full)
    }

    // Ranged/capped segment: never write past the effective end. When dynamic
    // re-segmentation lowers the cap, we stop here and flag a clean finish so
    // the engine hands the tail to a new segment.
    const std::int64_t want = self->capEnd_ - pos + 1;  // bytes still wanted
    if (want <= 0) {
        self->cappedComplete_ = true;
        return 0;  // nothing more wanted -> abort cleanly (capped)
    }
    const std::size_t writeN = (static_cast<std::int64_t>(total) <= want)
                                   ? total
                                   : static_cast<std::size_t>(want);
    const ssize_t w = ::pwrite(self->fd_, ptr, writeN, offset);
    if (w <= 0) return 0;  // disk error -> WRITE_ERROR
    self->bytesWrittenSoFar_ += static_cast<std::int64_t>(w);
    if (static_cast<std::size_t>(w) < writeN) {
        return static_cast<std::size_t>(w);  // disk short-write -> WRITE_ERROR (real error, not capped)
    }
    if (writeN < total) {
        // Wrote exactly up to the cap; short-return to stop the transfer.
        self->cappedComplete_ = true;
    }
    return static_cast<std::size_t>(w);  // == total: continue; < total: clean capped stop
}

}  // namespace fdm
