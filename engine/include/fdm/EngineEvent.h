#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "fdm/ChunkSpec.h"

namespace fdm {

// Per-chunk live progress, populated for every Progress event.
struct ChunkProgress {
    enum class Status { Active, Done, Failed };

    int index = 0;
    std::int64_t startByte = 0;
    std::int64_t endByte = -1;        // -1 if chunk is the whole-file fallback
    std::int64_t bytesReceived = 0;
    int attempts = 1;                 // 1 = first try, 2 = first retry, etc.
    Status status = Status::Active;
};

struct Started {
    std::int64_t contentLength = -1;
    bool supportsRanges = false;
    int chunkCount = 1;
    std::vector<ChunkSpec> chunks;    // per-chunk byte ranges, indexed by ChunkSpec::index
};

struct Progress {
    std::int64_t received = 0;
    std::int64_t total = -1;
    double bytesPerSec = 0.0;         // instantaneous rate since the previous Progress event
    std::vector<ChunkProgress> chunks;
};

struct Finished {};

struct Failed {
    std::string reason;
};

struct Paused {};

using EngineEvent = std::variant<Started, Progress, Finished, Failed, Paused>;

}  // namespace fdm
