#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "fdm/ChunkSpec.h"

namespace fdm {

// Per-segment live progress, populated for every Progress event.
struct ChunkProgress {
    // Active   = downloading right now.
    // Pending  = waiting for a free connection slot, or backing off after a
    //            transient error (429 / reset / timeout) before being retried.
    // Done     = this byte range is fully on disk.
    // Failed   = a fatal error on this segment (kills the whole download).
    enum class Status { Active, Pending, Done, Failed };

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
    // Server-advertised content hash captured during probe (Content-Digest,
    // Digest, or Content-MD5). Empty when the server didn't provide one.
    std::string expectedHash;         // lowercased hex
    std::string hashAlgorithm;        // "sha256" | "sha1" | "md5"
    std::string hashSource;           // "content-digest" | "digest" | "content-md5"
    // If-Range validator (strong ETag or Last-Modified) captured at probe
    // time. Persist it and replay it via ResumeSpec::validator so a resumed
    // download notices when the remote file changed. Empty if unavailable.
    std::string validator;
};

struct Progress {
    std::int64_t received = 0;
    std::int64_t total = -1;
    double bytesPerSec = 0.0;         // instantaneous rate since the previous Progress event
    std::vector<ChunkProgress> chunks;
    int activeConnections = 0;        // segments downloading right now
    int connectionLimit = 0;          // current adaptive cap on parallel connections
};

struct Finished {};

struct Failed {
    std::string reason;
};

struct Paused {};

using EngineEvent = std::variant<Started, Progress, Finished, Failed, Paused>;

}  // namespace fdm
