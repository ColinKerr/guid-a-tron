/*---------------------------------------------------------------------------------------------
 * guid-a-tron — file operations. See Files.h.
 *--------------------------------------------------------------------------------------------*/
#include "Files.h"

#include <algorithm>
#include <filesystem>

namespace guidatron {

namespace fs = std::filesystem;

//---------------------------------------------------------------------------------------------
bool FileExists(std::string const& path) {
    std::error_code ec;
    return fs::exists(fs::path(path), ec);
}

//---------------------------------------------------------------------------------------------
bool CopyFileFailIfPresent(std::string const& from, std::string const& to, std::string& error) {
    if (FileExists(to)) {
        error = "output already exists: " + to;
        return false;
    }
    if (!FileExists(from)) {
        error = "input not found: " + from;
        return false;
    }
    std::error_code ec;
    // copy_options::none means "fail if the target exists", which is the point.
    if (!fs::copy_file(fs::path(from), fs::path(to), fs::copy_options::none, ec)) {
        error = "copy failed: " + ec.message();
        return false;
    }
    return true;
}

//---------------------------------------------------------------------------------------------
bool MoveFileOver(std::string const& from, std::string const& to) {
    std::error_code ec;
    fs::rename(fs::path(from), fs::path(to), ec);
    return !ec;
}

//---------------------------------------------------------------------------------------------
bool RemoveFileIfPresent(std::string const& path) {
    std::error_code ec;
    fs::remove(fs::path(path), ec);
    return !ec;
}

//---------------------------------------------------------------------------------------------
bool CreateDirectories(std::string const& path) {
    if (path.empty())
        return true;
    std::error_code ec;
    fs::create_directories(fs::path(path), ec);
    return !ec || FileExists(path);
}

//---------------------------------------------------------------------------------------------
bool FileSize(std::string const& path, uint64_t& size) {
    size = 0;
    std::error_code ec;
    uintmax_t value = fs::file_size(fs::path(path), ec);
    if (ec)
        return false;
    size = (uint64_t)value;
    return true;
}

//---------------------------------------------------------------------------------------------
std::string ParentDir(std::string const& path) { return fs::path(path).parent_path().string(); }

//---------------------------------------------------------------------------------------------
std::string BaseName(std::string const& path) { return fs::path(path).filename().string(); }

//---------------------------------------------------------------------------------------------
std::vector<std::string> ListFiles(std::string const& dir, std::string const& suffix) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto const& entry : fs::directory_iterator(fs::path(dir), ec)) {
        std::string path = entry.path().string();
        if (suffix.empty() ||
            (path.size() >= suffix.size() && 0 == path.compare(path.size() - suffix.size(),
                                                               suffix.size(), suffix)))
            out.push_back(path);
    }
    std::sort(out.begin(), out.end());
    return out;
}

//---------------------------------------------------------------------------------------------
std::string TempDir() {
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    return ec ? std::string("/tmp") : dir.string();
}

} // namespace guidatron
