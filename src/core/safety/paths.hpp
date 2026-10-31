#pragma once

#include <filesystem>
#include <string>

#include "core/model/result.hpp"

namespace myclean::safety {

// The one spelling SafeRemove matches and acts on: "C:\dir\file". Removes a "\\?\" prefix,
// turns "/" into "\", applies "." and "..", drops repeated and trailing separators and the
// trailing dots and spaces of every component (as Win32 does), and upper-cases the drive letter.
// Fails with invalid_argument for anything that is not an absolute drive path: relative, UNC
// or device paths, stream names (":"), wildcards and components that end up empty.
// 8.3 short names need the file system: see os::IFileSystem::expand_short_names.
[[nodiscard]] Result<std::filesystem::path> normalize_path(const std::filesystem::path& path);

// UTF-8 text of a path, for reasons and messages.
[[nodiscard]] std::string to_utf8(const std::filesystem::path& path);

}  // namespace myclean::safety
