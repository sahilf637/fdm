#pragma once

#include <curl/curl.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "fdm/ChunkSpec.h"
#include "fdm/DownloadEngine.h"

namespace fdm {

// Internal. One chunk of a download. Owns one curl easy handle and one FILE*
// opened in r+b mode at startByte. Lifetime is owned by the engine for the
// duration of the download.
class ChunkTask {
public:
    // initialBytesReceived = bytes already written for this chunk (only set
    // when restoring from persistent state). The constructor advances the
    // file pointer past them and configures CURLOPT_RANGE to resume after
    // them; `attempts` is the attempts counter to restore (1 if fresh).
    // `headers` are extra request headers (raw "Name: value" lines) sent on
    // this chunk's request. `validator` (strong ETag or Last-Modified) is
    // sent as If-Range alongside any Range request, so a changed resource
    // returns 200 instead of mixing bytes from two versions.
    ChunkTask(std::string url, std::string outputPath, ChunkSpec spec,
              std::vector<std::string> headers = {},
              std::int64_t initialBytesReceived = 0, int initialAttempts = 1,
              const std::string& validator = {});
    ~ChunkTask();

    ChunkTask(const ChunkTask&) = delete;
    ChunkTask& operator=(const ChunkTask&) = delete;

    // Hand the easy handle to the engine. onComplete fires on the engine
    // thread when libcurl reports CURLMSG_DONE for this handle.
    void start(DownloadEngine& engine, std::function<void(CURLcode)> onComplete);

    // Lower this chunk's effective end (dynamic re-segmentation): the write
    // callback stops once it has written up to `newEnd` (inclusive), at which
    // point the transfer aborts cleanly and cappedComplete() returns true. The
    // tail past newEnd is handed to a new segment. Engine-thread only.
    void capEndAt(std::int64_t newEnd) { capEnd_ = newEnd; }

    // True when the transfer ended because it reached the (possibly lowered)
    // end cap -- i.e. this byte range is fully on disk, not a failure.
    bool cappedComplete() const { return cappedComplete_; }

    std::int64_t bytesWritten() const { return bytesWrittenSoFar_; }
    const ChunkSpec& spec() const { return spec_; }
    int attempts() const { return attempts_; }
    CURL* easy() const { return easy_; }

    // True when a ranged request came back as 200 (full body) and the write
    // callback aborted it: the server either ignores Range or the resource
    // changed (If-Range mismatch). Distinguishes that fatal case from a
    // generic CURLE_WRITE_ERROR (disk error).
    bool sawFullBodyForRange() const { return sawFullBodyForRange_; }

    // Retry-After value (seconds) parsed from the response headers, or -1 if
    // the server didn't send one / sent the HTTP-date form.
    long retryAfterSecs() const { return retryAfterSecs_; }

private:
    static std::size_t writeCallback(char* ptr, std::size_t size, std::size_t nmemb, void* userp);
    static std::size_t headerCallback(char* buf, std::size_t size, std::size_t nmemb, void* userp);

    std::string url_;
    std::string outputPath_;
    ChunkSpec spec_;
    int fd_ = -1;  // owned; closed in dtor. Written via pwrite at absolute offsets.
    CURL* easy_ = nullptr;
    struct curl_slist* headers_ = nullptr;  // owned; freed in dtor
    EasyContext ctx_;
    std::int64_t bytesWrittenSoFar_ = 0;
    int attempts_ = 1;  // counts attempts including the first
    std::int64_t capEnd_ = -1;       // effective inclusive end; -1 = open-ended
    bool cappedComplete_ = false;    // reached the cap (clean finish, not an error)
    bool sawFullBodyForRange_ = false;
    long retryAfterSecs_ = -1;       // from Retry-After (seconds form only)
    // Start offset parsed from the 206 "Content-Range: bytes A-B/T" header;
    // -1 until seen. Compared against the offset we asked for before the
    // first write so a broken server/proxy answering with the wrong range
    // can't land bytes at the wrong position.
    std::int64_t contentRangeStart_ = -1;
    bool rangeChecked_ = false;
    // Open-ended segment resumed with bytes already on disk: a Range request
    // was sent on a best-effort basis. Checked (and cleared) on the first
    // write to see whether the server honored it (206) or restarted (200).
    bool openEndedResume_ = false;
};

}  // namespace fdm
