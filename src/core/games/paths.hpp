#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// Path comparisons of game detection: Windows paths compared as Windows compares them, ASCII
// letters case-insensitively, so "E:\Jogos" holds "e:\jogos\title\title.exe".
namespace myclean::games {

// The comparison key of `path`: lexically normal, '\' separators, the "\\?\" prefix removed,
// ASCII letters folded to lower case and no trailing separator except at a volume root ("e:\").
[[nodiscard]] std::wstring path_key(const std::filesystem::path& path);

// True when both name the same entry, ignoring the case of ASCII letters.
[[nodiscard]] bool same_path(const std::filesystem::path& left, const std::filesystem::path& right);

// True when `path` is `root` or lies inside it at a folder boundary: "E:\Jogos" holds
// "E:\Jogos\x.exe" but not "E:\Jogos2\x.exe". An empty root holds nothing.
[[nodiscard]] bool path_within(const std::filesystem::path& path,
                               const std::filesystem::path& root);

// True when a folder of `path` is named "WindowsApps": the Store's package folder, which holds
// Store Python and winget and is never a game library (spike X5 §2).
[[nodiscard]] bool in_windows_apps(const std::filesystem::path& path);

// UTF-8 text as a path, and a path as UTF-8 text.
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view text);
[[nodiscard]] std::string utf8(const std::filesystem::path& path);

}  // namespace myclean::games
