#include "fdm/Paths.h"

#include <filesystem>
#include <string>

namespace fdm {

std::string findAvailablePath(const std::string& path) {
    namespace fs = std::filesystem;
    fs::path p(path);
    std::error_code ec;
    if (!fs::exists(p, ec)) return path;

    const fs::path dir = p.parent_path();
    const std::string stem = p.stem().string();
    const std::string ext = p.extension().string();  // includes leading '.', or "" if none

    for (int i = 1; i <= 9999; ++i) {
        const std::string candidate = stem + " (" + std::to_string(i) + ")" + ext;
        const fs::path candidatePath = dir / candidate;
        if (!fs::exists(candidatePath, ec)) return candidatePath.string();
    }
    return path;
}

}  // namespace fdm
