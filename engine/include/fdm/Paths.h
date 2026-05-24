#pragma once

#include <string>

namespace fdm {

// If `path` doesn't exist, return it unchanged. Otherwise return the first
// "stem (N)ext" variant (N starting at 1) that does not exist on disk. The
// suffix is inserted between the file's stem and its final extension, e.g.
// "/tmp/file.iso"     -> "/tmp/file (1).iso"
// "/tmp/file.tar.gz"  -> "/tmp/file.tar (1).gz"  (last dot only)
// "/tmp/file"         -> "/tmp/file (1)"
// Gives up at N=9999 and returns the original path; callers can decide what
// to do (in practice this never happens).
std::string findAvailablePath(const std::string& path);

}  // namespace fdm
