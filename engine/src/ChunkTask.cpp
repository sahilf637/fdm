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
    const int fd = ::open(outputPath_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        throw std::runtime_error(std::string("open ") + outputPath_ + ": " +
                                 std::strerror(errno));
    }
    file_ = ::fdopen(fd, "r+b");
    if (!file_) {
        const int err = errno;
        ::close(fd);
        throw std::runtime_error(std::string("fdopen: ") + std::strerror(err));
    }
    // Seek to where the next byte will be written -- chunk start plus any
    // bytes already on disk from a previous run.
    const std::int64_t writeOffset = spec_.startByte + bytesWrittenSoFar_;
    if (::fseeko(file_, static_cast<off_t>(writeOffset), SEEK_SET) != 0) {
        const int err = errno;
        std::fclose(file_);
        file_ = nullptr;
        throw std::runtime_error(std::string("fseeko: ") + std::strerror(err));
    }

    easy_ = curl_easy_init();
    if (!easy_) {
        std::fclose(file_);
        file_ = nullptr;
        throw std::runtime_error("curl_easy_init failed");
    }

    curl_easy_setopt(easy_, CURLOPT_URL, url_.c_str());
    if (spec_.endByte >= 0) {
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
}

ChunkTask::~ChunkTask() {
    if (easy_) curl_easy_cleanup(easy_);
    if (headers_) curl_slist_free_all(headers_);
    if (file_) std::fclose(file_);
}

void ChunkTask::start(DownloadEngine& engine, std::function<void(CURLcode)> onComplete) {
    // Flush the FILE* buffer before notifying the caller so a downstream reader
    // sees the bytes immediately. fclose in the dtor would also flush, but the
    // caller usually checks the file before destroying the task.
    ctx_.onDone = [this, cb = std::move(onComplete)](CURLcode rc) {
        if (file_) std::fflush(file_);
        if (cb) cb(rc);
    };
    curl_easy_setopt(easy_, CURLOPT_PRIVATE, &ctx_);
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

    // Open-ended segment (unknown size / no ranges): write everything.
    if (self->capEnd_ < 0) {
        const std::size_t w = std::fwrite(ptr, 1, total, self->file_);
        self->bytesWrittenSoFar_ += static_cast<std::int64_t>(w);
        return w;  // short write -> WRITE_ERROR (disk error)
    }

    // Ranged/capped segment: never write past the effective end. When dynamic
    // re-segmentation lowers the cap, we stop here and flag a clean finish so
    // the engine hands the tail to a new segment.
    const std::int64_t pos = self->spec_.startByte + self->bytesWrittenSoFar_;
    const std::int64_t want = self->capEnd_ - pos + 1;  // bytes still wanted
    if (want <= 0) {
        self->cappedComplete_ = true;
        return 0;  // nothing more wanted -> abort cleanly (capped)
    }
    const std::size_t writeN = (static_cast<std::int64_t>(total) <= want)
                                   ? total
                                   : static_cast<std::size_t>(want);
    const std::size_t w = std::fwrite(ptr, 1, writeN, self->file_);
    self->bytesWrittenSoFar_ += static_cast<std::int64_t>(w);
    if (w < writeN) {
        return w;  // disk short-write -> WRITE_ERROR (real error, not capped)
    }
    if (writeN < total) {
        // Wrote exactly up to the cap; short-return to stop the transfer.
        self->cappedComplete_ = true;
    }
    return w;  // == total: continue; < total: clean capped stop
}

}  // namespace fdm
