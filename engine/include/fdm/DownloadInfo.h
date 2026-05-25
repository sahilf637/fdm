#pragma once

#include <cstdint>
#include <string>

namespace fdm {

struct DownloadInfo {
    std::string finalUrl;
    std::int64_t contentLength = -1;
    bool supportsRanges = false;
    // Server-suggested filename, drawn from Content-Disposition (RFC 6266).
    // Empty if the server didn't supply one (or supplied an unparseable /
    // unsafe value). Already basename-sanitized: path separators stripped,
    // control characters removed, so it's safe to use as a leaf filename.
    std::string suggestedFilename;
    // Server-advertised content hash, sourced from (in priority order)
    // Content-Digest (RFC 9530), Digest (RFC 3230), or Content-MD5 (legacy).
    // When multiple algorithms are listed, the strongest wins:
    // SHA-256 > SHA-1 > MD5. Lowercased hex, empty if no usable digest.
    std::string expectedHash;
    std::string hashAlgorithm;  // "sha256" | "sha1" | "md5"
    std::string hashSource;     // "content-digest" | "digest" | "content-md5"
};

}  // namespace fdm
