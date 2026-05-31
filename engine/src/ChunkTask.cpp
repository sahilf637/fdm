#include "ChunkTask.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace fdm {

ChunkTask::ChunkTask(std::string url, std::string outputPath, ChunkSpec spec,
                     std::vector<std::string> headers,
                     std::int64_t initialBytesReceived, int initialAttempts)
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
    if (spec_.endByte >= 0) {
        // Resume past any bytes already on disk from a previous run.
        const std::int64_t writeOffset = spec_.startByte + bytesWrittenSoFar_;
        const std::string range = std::to_string(writeOffset) + "-" +
                                  std::to_string(spec_.endByte);
        curl_easy_setopt(easy_, CURLOPT_RANGE, range.c_str());
    }
    curl_easy_setopt(easy_, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy_, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(easy_, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(easy_, CURLOPT_WRITEFUNCTION, &ChunkTask::writeCallback);
    curl_easy_setopt(easy_, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(easy_, CURLOPT_USERAGENT, "fdm/0.1");
    // Forward caller-supplied headers (Cookie / Referer / a real browser
    // User-Agent). A User-Agent here overrides the default set just above.
    for (const std::string& h : headers) {
        if (!h.empty()) headers_ = curl_slist_append(headers_, h.c_str());
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

bool ChunkTask::prepareRetry() {
    if (attempts_ >= kMaxAttempts) return false;
    attempts_++;

    const std::int64_t resumeStart = spec_.startByte + bytesWrittenSoFar_;
    if (spec_.endByte >= 0) {
        if (resumeStart > spec_.endByte) {
            // We've already received everything; nothing left to retry.
            return false;
        }
        const std::string range = std::to_string(resumeStart) + "-" +
                                  std::to_string(spec_.endByte);
        curl_easy_setopt(easy_, CURLOPT_RANGE, range.c_str());
    }
    // ctx_ (and thus CURLOPT_PRIVATE) is still valid; the same onDone fires.
    return true;
}

bool ChunkTask::reconfigureForResume() {
    const std::int64_t resumeStart = spec_.startByte + bytesWrittenSoFar_;
    if (spec_.endByte >= 0) {
        if (resumeStart > spec_.endByte) return false;
        const std::string range = std::to_string(resumeStart) + "-" +
                                  std::to_string(spec_.endByte);
        curl_easy_setopt(easy_, CURLOPT_RANGE, range.c_str());
    }
    return true;
}

std::size_t ChunkTask::writeCallback(char* ptr, std::size_t size, std::size_t nmemb,
                                     void* userp) {
    auto* self = static_cast<ChunkTask*>(userp);
    const std::size_t total = size * nmemb;

    // Defense against servers that lie about range support: if we asked for a
    // Range but the server returned 200 (full body) instead of 206, writing
    // here would corrupt the file at this chunk's offset. Abort by returning
    // a short write -> libcurl raises CURLE_WRITE_ERROR -> the engine treats
    // it as a chunk failure.
    if (self->spec_.endByte >= 0) {
        long http = 0;
        curl_easy_getinfo(self->easy_, CURLINFO_RESPONSE_CODE, &http);
        if (http == 200) return 0;
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
