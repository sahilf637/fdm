#pragma once

#include <curl/curl.h>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include "fdm/ChunkSpec.h"
#include "fdm/DownloadEngine.h"

namespace fdm {

// Internal. One chunk of a download. Owns one curl easy handle and one FILE*
// opened in r+b mode at startByte. Lifetime is owned by the engine for the
// duration of the download.
class ChunkTask {
public:
    static constexpr int kMaxAttempts = 4;

    // initialBytesReceived = bytes already written for this chunk (only set
    // when restoring from persistent state). The constructor advances the
    // file pointer past them and configures CURLOPT_RANGE to resume after
    // them; `attempts` is the attempts counter to restore (1 if fresh).
    ChunkTask(std::string url, std::string outputPath, ChunkSpec spec,
              std::int64_t initialBytesReceived = 0, int initialAttempts = 1);
    ~ChunkTask();

    ChunkTask(const ChunkTask&) = delete;
    ChunkTask& operator=(const ChunkTask&) = delete;

    // Hand the easy handle to the engine. onComplete fires on the engine
    // thread when libcurl reports CURLMSG_DONE for this handle.
    void start(DownloadEngine& engine, std::function<void(CURLcode)> onComplete);

    // Configure the easy handle for a resumed attempt: adjusts CURLOPT_RANGE
    // to (startByte + bytesWritten)-endByte. Returns true if a retry slot is
    // available (attempts < kMaxAttempts), false if exhausted.
    bool prepareRetry();

    // Like prepareRetry, but without consuming a retry attempt. Used by
    // DownloadEngine::resume to pick up where pause left off. Returns true
    // if there is still work to do (resumeStart <= endByte), false if the
    // chunk is already complete.
    bool reconfigureForResume();

    std::int64_t bytesWritten() const { return bytesWrittenSoFar_; }
    const ChunkSpec& spec() const { return spec_; }
    int attempts() const { return attempts_; }
    CURL* easy() const { return easy_; }

private:
    static std::size_t writeCallback(char* ptr, std::size_t size, std::size_t nmemb, void* userp);

    std::string url_;
    std::string outputPath_;
    ChunkSpec spec_;
    std::FILE* file_ = nullptr;
    CURL* easy_ = nullptr;
    EasyContext ctx_;
    std::int64_t bytesWrittenSoFar_ = 0;
    int attempts_ = 1;  // counts attempts including the first
};

}  // namespace fdm
