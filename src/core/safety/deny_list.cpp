#include "core/safety/deny_list.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace myclean::safety {

namespace {

namespace fs = std::filesystem;

// %WINDIR% subfolders whose contents (not the folders themselves) may be removed.
constexpr std::array<std::wstring_view, 1> windows_allow_list = {L"Temp"};

std::wstring folded(const fs::path& path) {
  std::wstring text = path.native();
  for (wchar_t& character : text) {
    if (character >= L'A' && character <= L'Z') {
      character = static_cast<wchar_t>(character - L'A' + L'a');
    }
  }
  return text;
}

// True when `path` is `base` or lies inside it. An empty base contains nothing.
bool within(const std::wstring& path, const std::wstring& base) {
  if (base.empty() || !path.starts_with(base)) {
    return false;
  }
  return path.size() == base.size() || base.ends_with(L'\\') || path[base.size()] == L'\\';
}

bool within(const fs::path& path, const fs::path& base) {
  return within(folded(path), folded(base));
}

// True when `path` lies strictly inside `base`.
bool inside(const fs::path& path, const fs::path& base) {
  return within(path, base) && folded(path).size() > folded(base).size();
}

// True when `path` is `base`, lies inside it, or contains it.
bool overlaps(const fs::path& path, const fs::path& base) {
  const std::wstring one = folded(path);
  const std::wstring other = folded(base);
  return within(one, other) || within(other, one);
}

bool overlaps_any(const fs::path& path, std::span<const fs::path> bases) {
  return std::ranges::any_of(bases, [&path](const fs::path& base) { return overlaps(path, base); });
}

bool is_volume_root(const fs::path& path, const ProtectedLocations& /*locations*/) {
  return !path.has_relative_path();
}

// The first folder or file below the volume root, case-folded; empty for a volume root.
std::wstring top_level(const fs::path& path) {
  const fs::path relative = path.relative_path();
  return relative.empty() ? std::wstring{} : folded(*relative.begin());
}

bool is_system_volume_information(const fs::path& path, const ProtectedLocations& /*locations*/) {
  return top_level(path) == L"system volume information";
}

bool is_in_recovery(const fs::path& path, const ProtectedLocations& /*locations*/) {
  return top_level(path) == L"recovery";
}

bool is_in_boot(const fs::path& path, const ProtectedLocations& /*locations*/) {
  return top_level(path) == L"boot";
}

bool is_page_file(const fs::path& path, const ProtectedLocations& /*locations*/) {
  const std::wstring name = top_level(path);
  return name == L"pagefile.sys" || name == L"hiberfil.sys" || name == L"swapfile.sys";
}

// $Recycle.Bin, $WinREAgent, $Extend, $SysReset, $Windows.~BT and the like.
bool is_in_root_dollar_entry(const fs::path& path, const ProtectedLocations& /*locations*/) {
  return top_level(path).starts_with(L'$');
}

bool is_in_windows(const fs::path& path, const ProtectedLocations& locations) {
  if (!overlaps(path, locations.windows)) {
    return false;
  }
  return std::ranges::none_of(windows_allow_list, [&](std::wstring_view allowed) {
    return inside(path, locations.windows / allowed);
  });
}

bool is_in_program_files(const fs::path& path, const ProtectedLocations& /*locations*/) {
  return top_level(path).starts_with(L"program files");
}

bool is_in_git(const fs::path& path, const ProtectedLocations& /*locations*/) {
  return std::ranges::any_of(
      path.relative_path(), [](const fs::path& component) { return folded(component) == L".git"; });
}

bool is_in_user_known_folder(const fs::path& path, const ProtectedLocations& locations) {
  return overlaps_any(path, locations.owner_known_folders);
}

bool is_in_other_profile(const fs::path& path, const ProtectedLocations& locations) {
  if (within(locations.profiles, path)) {
    return true;  // the profiles folder itself, or a folder that contains it
  }
  return within(path, locations.profiles) && !within(path, locations.owner_profile);
}

bool is_in_myclean(const fs::path& path, const ProtectedLocations& locations) {
  return overlaps_any(path, locations.myclean);
}

constexpr std::array<DenyRule, 12> rules = {{
    {.name = "volume_root", .matches = is_volume_root},
    {.name = "system_volume_information", .matches = is_system_volume_information},
    {.name = "recovery", .matches = is_in_recovery},
    {.name = "boot", .matches = is_in_boot},
    {.name = "page_file", .matches = is_page_file},
    {.name = "root_dollar_entry", .matches = is_in_root_dollar_entry},
    {.name = "windir", .matches = is_in_windows},
    {.name = "program_files", .matches = is_in_program_files},
    {.name = "git", .matches = is_in_git},
    {.name = "user_known_folder", .matches = is_in_user_known_folder},
    {.name = "other_profile", .matches = is_in_other_profile},
    {.name = "myclean", .matches = is_in_myclean},
}};

}  // namespace

std::span<const DenyRule> deny_list() {
  return rules;
}

std::optional<std::string_view> denied_by(const fs::path& path,
                                          const ProtectedLocations& locations) {
  for (const DenyRule& rule : rules) {
    if (rule.matches(path, locations)) {
      return rule.name;
    }
  }
  return std::nullopt;
}

}  // namespace myclean::safety
