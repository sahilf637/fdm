#include "doctest.h"

#include <cstdint>

#include "fdm/ChunkSpec.h"

using fdm::ChunkSpec;
using fdm::splitIntoChunks;

constexpr std::int64_t kKiB = 1024;
constexpr std::int64_t kMiB = 1024 * kKiB;

// Helper: assert chunks tile [0, totalSize-1] with no gaps or overlaps.
static void requireTiling(const std::vector<ChunkSpec>& chunks, std::int64_t totalSize) {
    REQUIRE(!chunks.empty());
    CHECK(chunks.front().startByte == 0);
    CHECK(chunks.back().endByte == totalSize - 1);
    for (std::size_t i = 1; i < chunks.size(); ++i) {
        CHECK(chunks[i].startByte == chunks[i - 1].endByte + 1);
    }
    std::int64_t totalBytes = 0;
    for (const auto& c : chunks) totalBytes += (c.endByte - c.startByte + 1);
    CHECK(totalBytes == totalSize);
}

TEST_CASE("unknown size returns a single sentinel chunk") {
    const auto chunks = splitIntoChunks(-1, 8, 1 * kMiB);
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0].startByte == 0);
    CHECK(chunks[0].endByte == -1);
    CHECK(chunks[0].index == 0);
}

TEST_CASE("zero size returns an empty vector") {
    const auto chunks = splitIntoChunks(0, 8, 1 * kMiB);
    CHECK(chunks.empty());
}

TEST_CASE("file smaller than minChunkSize -> single chunk") {
    const auto chunks = splitIntoChunks(500, 8, 1 * kMiB);
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0].startByte == 0);
    CHECK(chunks[0].endByte == 499);
    requireTiling(chunks, 500);
}

TEST_CASE("file equal to minChunkSize -> single chunk") {
    const auto chunks = splitIntoChunks(1 * kMiB, 8, 1 * kMiB);
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0].endByte == 1 * kMiB - 1);
}

TEST_CASE("maxChunks == 1 always yields one chunk") {
    const auto chunks = splitIntoChunks(100 * kMiB, 1, 1 * kMiB);
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0].endByte == 100 * kMiB - 1);
}

TEST_CASE("exact multiple of n: 8 MiB / 8 -> 8 equal chunks of 1 MiB") {
    const auto chunks = splitIntoChunks(8 * kMiB, 8, 1 * kMiB);
    REQUIRE(chunks.size() == 8);
    for (int i = 0; i < 8; ++i) {
        CHECK(chunks[i].index == i);
        CHECK(chunks[i].startByte == static_cast<std::int64_t>(i) * kMiB);
        CHECK(chunks[i].endByte == static_cast<std::int64_t>(i + 1) * kMiB - 1);
    }
    requireTiling(chunks, 8 * kMiB);
}

TEST_CASE("non-multiple of n: 10 MiB / 8 -> last chunk absorbs the remainder") {
    const std::int64_t total = 10 * kMiB;
    const auto chunks = splitIntoChunks(total, 8, 1 * kMiB);
    REQUIRE(chunks.size() == 8);
    const std::int64_t base = total / 8;  // 1310720 (= 1.25 MiB exact here)
    for (int i = 0; i < 7; ++i) {
        CHECK(chunks[i].endByte - chunks[i].startByte + 1 == base);
    }
    CHECK(chunks.back().endByte == total - 1);
    requireTiling(chunks, total);
}

TEST_CASE("size only big enough for 2 chunks even with max=8") {
    // totalSize / minChunkSize = (1 MiB + 1) / 512 KiB = 2  ->  n = 2
    const std::int64_t total = 1 * kMiB + 1;
    const auto chunks = splitIntoChunks(total, 8, 512 * kKiB);
    REQUIRE(chunks.size() == 2);
    requireTiling(chunks, total);
}

TEST_CASE("byMin caps n even when maxChunks is larger") {
    // 3 MiB with min=1 MiB -> byMin = 3, n = 3 (not 8)
    const auto chunks = splitIntoChunks(3 * kMiB, 8, 1 * kMiB);
    REQUIRE(chunks.size() == 3);
    requireTiling(chunks, 3 * kMiB);
}

TEST_CASE("maxChunks caps n when file is huge") {
    // 100 MiB with min=1 MiB would give byMin=100; cap at 8.
    const auto chunks = splitIntoChunks(100 * kMiB, 8, 1 * kMiB);
    REQUIRE(chunks.size() == 8);
    requireTiling(chunks, 100 * kMiB);
}
