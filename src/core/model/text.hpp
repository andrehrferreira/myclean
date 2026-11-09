#pragma once

#include <filesystem>
#include <string>

namespace myclean {

// The UTF-8 text of `path`. Never throws: NTFS allows names that are not valid UTF-16 (a lone
// surrogate, as in some Recycle Bin entries), which std::filesystem::path::u8string() rejects
// with an exception; each such unit becomes U+FFFD, as Explorer shows it. The text then names a
// file no API can open, so an action on it fails as "not found" instead of reaching another file.
[[nodiscard]] std::string utf8_text(const std::filesystem::path& path);

}  // namespace myclean
