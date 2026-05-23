#include "fdm/ChunkSpec.h"

#include <algorithm>

namespace fdm {

std::vector<ChunkSpec> splitIntoChunks(std::int64_t totalSize,
                                       int maxChunks,
                                       std::int64_t minChunkSize) {
    if (totalSize < 0) {
        return {ChunkSpec{0, -1, 0}};
    }
    if (totalSize == 0) {
        return {};
    }
    if (totalSize <= minChunkSize || maxChunks <= 1) {
        return {ChunkSpec{0, totalSize - 1, 0}};
    }

    const std::int64_t byMin = totalSize / minChunkSize;
    const int n = static_cast<int>(
        std::min<std::int64_t>(maxChunks, std::max<std::int64_t>(1, byMin)));

    std::vector<ChunkSpec> chunks;
    chunks.reserve(static_cast<std::size_t>(n));
    const std::int64_t base = totalSize / n;
    for (int i = 0; i < n; ++i) {
        const std::int64_t start = static_cast<std::int64_t>(i) * base;
        const std::int64_t end = (i == n - 1) ? (totalSize - 1) : (start + base - 1);
        chunks.push_back(ChunkSpec{start, end, i});
    }
    return chunks;
}

}  // namespace fdm
