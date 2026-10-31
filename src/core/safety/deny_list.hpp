#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace myclean::safety {

// The machine-specific folders the deny-list protects. Filled by the platform layer from the
// operating system (platform::protected_locations), never from rule files.
struct ProtectedLocations {
  std::filesystem::path windows;        // %WINDIR%
  std::filesystem::path profiles;       // the folder holding every user profile (C:\Users)
  std::filesystem::path owner_profile;  // empty when there is no owner: every profile is foreign
  // The owner's Documents, Desktop, Pictures, Videos and Music, wherever they are redirected.
  std::vector<std::filesystem::path> owner_known_folders;
  std::vector<std::filesystem::path> myclean;  // MyClean's data and install folders
};

// One entry of the hard deny-list. An item it matches is refused with "deny_list:<name>".
// A protected folder is matched with everything inside it and every folder that contains it.
struct DenyRule {
  std::string_view name;
  bool (*matches)(const std::filesystem::path& path, const ProtectedLocations& locations);
};

// Every entry, in evaluation order. It is compiled in: no rule or setting can change it.
//   volume_root        a volume root (D:\ and the like)
//   system_volume_information, recovery, boot
//                      that top-level folder of any volume, with everything inside it
//   page_file          pagefile.sys, hiberfil.sys or swapfile.sys at any volume root
//   root_dollar_entry  any top-level entry whose name starts with '$' ($Recycle.Bin,
//                      $WinREAgent, $Extend, ...), with everything inside it
//   windir             %WINDIR%, except the contents of allow-listed subfolders (Temp)
//   program_files      any top-level "Program Files*" folder of any volume
//   git                a .git folder or file, and anything inside one
//   user_known_folder  the owner's Documents, Desktop, Pictures, Videos and Music
//   other_profile      the profiles folder and every profile but the owner's
//   myclean            MyClean's own data and install folders
[[nodiscard]] std::span<const DenyRule> deny_list();

// The name of the first entry that matches `path`, if any. `path` and the locations must be
// normalized (normalize_path) and long-named, so that an ASCII case-insensitive comparison is
// exact.
[[nodiscard]] std::optional<std::string_view> denied_by(const std::filesystem::path& path,
                                                        const ProtectedLocations& locations);

}  // namespace myclean::safety
