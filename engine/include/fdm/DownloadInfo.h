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
};

}  // namespace fdm
