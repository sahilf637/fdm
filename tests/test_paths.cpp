#include "doctest.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "fdm/Paths.h"

namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir() : path_(fs::temp_directory_path() / "fdm_paths_test") {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }
    void touch(const std::string& name) {
        std::ofstream(path_ / name) << "x";
    }

private:
    fs::path path_;
};

}  // namespace

TEST_CASE("findAvailablePath: non-existent path is returned unchanged") {
    TempDir tmp;
    const std::string target = (tmp.path() / "fresh.iso").string();
    CHECK(fdm::findAvailablePath(target) == target);
}

TEST_CASE("findAvailablePath: existing file gets a (1) suffix before the extension") {
    TempDir tmp;
    tmp.touch("file.iso");
    const std::string target = (tmp.path() / "file.iso").string();
    const std::string out = fdm::findAvailablePath(target);
    CHECK(out == (tmp.path() / "file (1).iso").string());
}

TEST_CASE("findAvailablePath: walks through (1), (2), (3) until a free slot is found") {
    TempDir tmp;
    tmp.touch("file.iso");
    tmp.touch("file (1).iso");
    tmp.touch("file (2).iso");
    const std::string target = (tmp.path() / "file.iso").string();
    const std::string out = fdm::findAvailablePath(target);
    CHECK(out == (tmp.path() / "file (3).iso").string());
}

TEST_CASE("findAvailablePath: file with no extension still gets a suffix") {
    TempDir tmp;
    tmp.touch("README");
    const std::string target = (tmp.path() / "README").string();
    const std::string out = fdm::findAvailablePath(target);
    CHECK(out == (tmp.path() / "README (1)").string());
}

TEST_CASE("findAvailablePath: only the last dot is treated as the extension") {
    TempDir tmp;
    tmp.touch("archive.tar.gz");
    const std::string target = (tmp.path() / "archive.tar.gz").string();
    const std::string out = fdm::findAvailablePath(target);
    CHECK(out == (tmp.path() / "archive.tar (1).gz").string());
}
