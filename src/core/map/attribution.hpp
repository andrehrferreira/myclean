#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/rules/data_files.hpp"

// What the well-known locations of a volume are: the attribution.toml of each knowledge pack
// (docs/packs.md).
//
//   [[area]]
//   id       = "windows.hibernation"            # required, unique
//   paths    = ['?:\hiberfil.sys']              # required: absolute paths after %VARIABLE%
//                                               # expansion; '*' and '?' match within one
//                                               # component; a folder or a file
//   category = "page-hibernate"                 # required: see Category
//   label    = "Hibernation file"               # required: plain English
//   reclaim  = { kind = "setting", ref = "hibernation", note = "..." }
//            # required. kind: see ReclaimKind. ref is required for clean-rule (a clean case of
//            # the pack), handler (a Windows cleanup handler name), relocate (a relocation
//            # recipe of the pack, or a built-in target: placement::CacheKind, wsl, docker)
//            # and setting (hibernation, page_file or shadow_storage), and not allowed for the
//            # other kinds. note is optional.
namespace myclean::map {

enum class Category {
  system,
  windows_servicing,
  page_hibernate,
  restore_points,
  apps,
  games,
  dev_toolchains,
  dev_caches,
  dev_projects,
  build_artifacts,
  browser_caches,
  app_caches,
  vm_disks,
  user_documents,
  user_media,
  downloads,
  recycle_bin,
  other,
};

inline constexpr std::size_t category_count = 18;

// Text forms use '-' ("windows-servicing").
[[nodiscard]] std::string_view to_string(Category category);
[[nodiscard]] std::optional<Category> parse_category(std::string_view text);

enum class ReclaimKind {
  none,           // needed as it is
  clean_rule,     // a cleaning rule (ref: rule id)
  handler,        // a Windows cleanup handler (ref: handler name)
  relocate,       // moves to another drive (ref: placement::CacheKind text, wsl or docker)
  setting,        // a system setting (ref: hibernation | page_file | shadow_storage)
  uninstall,      // goes away with its program, game or toolchain
  manual_review,  // only the owner can decide; the note says how
};

[[nodiscard]] std::string_view to_string(ReclaimKind kind);
[[nodiscard]] std::optional<ReclaimKind> parse_reclaim_kind(std::string_view text);

// True for the kinds whose bytes count as reclaimable: every kind but none and manual-review.
[[nodiscard]] constexpr bool reclaimable(ReclaimKind kind) {
  return kind != ReclaimKind::none && kind != ReclaimKind::manual_review;
}

struct Reclaim {
  ReclaimKind kind = ReclaimKind::none;
  std::string ref;
  std::string note;
};

struct Area {
  std::string id;
  std::vector<std::wstring> paths;  // as written, variables not expanded
  Category category = Category::other;
  std::string label;
  Reclaim reclaim;
};

// Validates the whole file: every area or one Errc::invalid_data error naming the file, the area
// and the field. `file` is used only in messages.
[[nodiscard]] Result<std::vector<Area>> parse_attribution_file(std::string_view text,
                                                               const std::filesystem::path& file);

// <data_dir>/attribution.toml
[[nodiscard]] Result<std::vector<Area>> load_attribution(const rules::IDataFiles& files,
                                                         const std::filesystem::path& data_dir);

}  // namespace myclean::map
