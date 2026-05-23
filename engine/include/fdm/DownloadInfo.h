#pragma once

#include <cstdint>
#include <string>

namespace fdm {

struct DownloadInfo {
    std::string finalUrl;
    std::int64_t contentLength = -1;
    bool supportsRanges = false;
};

}  // namespace fdm
