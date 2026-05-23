#pragma once

#include <cstdint>
#include <vector>

namespace fdm {

// HTTP `Range: bytes=startByte-endByte` is inclusive on both ends. A chunk
// with endByte == -1 means "unknown end" -- used for the fallback path when
// Content-Length is unavailable (e.g., chunked transfer encoding).
struct ChunkSpec {
    std::int64_t startByte = 0;
    std::int64_t endByte = -1;
    int index = 0;
};

// Pure: no I/O, no curl. Returns chunks covering [0, totalSize-1] contiguously.
// - totalSize <  0          -> single chunk {0, -1, 0}    (unknown size)
// - totalSize == 0          -> empty vector               (nothing to download)
// - totalSize <= minChunkSize OR maxChunks <= 1 -> single chunk
// - otherwise               -> n chunks where n = min(maxChunks, totalSize / minChunkSize);
//                              first n-1 chunks have size `totalSize / n`, last chunk
//                              absorbs the remainder.
std::vector<ChunkSpec> splitIntoChunks(std::int64_t totalSize,
                                       int maxChunks,
                                       std::int64_t minChunkSize);

}  // namespace fdm
