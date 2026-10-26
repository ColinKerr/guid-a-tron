/*---------------------------------------------------------------------------------------------
 * guid-a-tron — the handful of file operations this tool performs, over std::filesystem.
 *
 * Paths are UTF-8 `std::string` throughout, which is what SQLite takes and what this tool prints.
 *
 * `CopyFileFailIfPresent` is the one with a rule behind it rather than a convenience: the input of
 * every command is opened read-only and never modified, so `seed` works on a copy, and refusing to
 * overwrite is what stops a re-run from quietly destroying the fork it is meant to extend.
 *--------------------------------------------------------------------------------------------*/
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace guidatron {

bool FileExists(std::string const& path);
//! Copy, refusing to overwrite. False with `error` set, and nothing written, on any refusal.
bool CopyFileFailIfPresent(std::string const& from, std::string const& to, std::string& error);
bool MoveFileOver(std::string const& from, std::string const& to);
//! Delete if present. False only if it existed and could not be removed.
bool RemoveFileIfPresent(std::string const& path);
bool CreateDirectories(std::string const& path);
bool FileSize(std::string const& path, uint64_t& size);
//! The directory part, or "" when there is none.
std::string ParentDir(std::string const& path);
//! The last component, which is what the per-changeset progress lines print.
std::string BaseName(std::string const& path);
std::string TempDir();
//! Every file in `dir` whose name ends in `suffix`, sorted by name. Empty if `dir` is not one.
std::vector<std::string> ListFiles(std::string const& dir, std::string const& suffix);

} // namespace guidatron
