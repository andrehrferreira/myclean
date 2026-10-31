#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/health/analysis.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"

// The vocabulary of `myclean scan-all` (scan-all spec): findings, the plan that would perform
// each one, and the drives they are on. Nothing here acts: a finding only names the action.
namespace myclean::scanall {

// What a finding would do, in precedence order: a byte several findings cover is counted under
// the first of these only (see build_report). defrag-trim frees no bytes.
enum class ActionGroup {
  remove,
  deduplicate,
  relocate,
  archive,
  compress,
  defrag_trim,
  uninstall_review,
  manual_review,
};

inline constexpr std::size_t action_group_count = 8;

// Text forms use '-' ("defrag-trim").
[[nodiscard]] std::string_view to_string(ActionGroup group);

// The MyClean action that would perform a finding, or what the owner would do where there is
// none. Text forms are the enumerator names.
enum class PlanKind {
  delete_items,      // actions::DeleteItems (cleaning rule, tier auto)
  quarantine_items,  // actions::QuarantineItems (tier ask; deduplicate: the extra copies)
  native_command,    // cleanup::NativeCommand (the rule's command)
  cleanup_handler,   // cleanup::HandlerCleanup (a Windows cleanup handler)
  compress_folder,   // compress::CompressFolder
  relocate_cache,    // placement::RelocateAction, through the cache's own setting
  hibernation,       // placement::HibernationAction
  page_file,         // placement::PageFileAction
  shadow_storage,    // placement::ShadowStorageAction
  retrim_volume,     // drives RetrimVolume
  defrag_volume,     // drives DefragVolume
  move_folder,       // no MyClean action: the owner (or the app's own mover) moves the folder
  uninstall,         // the program's own uninstaller, after the owner's review
  review,            // nothing automatic: the owner decides
};

[[nodiscard]] std::string_view to_string(PlanKind kind);

// The action and its parameters (UTF-8 names and values), as it would be built.
struct Plan {
  PlanKind kind = PlanKind::review;
  std::vector<std::pair<std::string, std::string>> params;

  // The value of parameter `name`; null when it is not set.
  [[nodiscard]] const std::string* param(std::string_view name) const;
};

// A folder or file a finding covers, with its size on disk.
struct Item {
  std::filesystem::path path;
  Bytes size;
};

struct Finding {
  ActionGroup group = ActionGroup::manual_review;
  std::string source;           // the analyzer: map, rules, artifacts, compress, maintenance,
                                // cold, placement or duplicates
  std::filesystem::path drive;  // the volume it frees space on ("C:\")
  std::filesystem::path path;   // what it acts on; empty for a whole volume
  // What it covers, by precedence (see build_report). Empty: nothing in the drive's tree, so
  // it is counted in full.
  std::vector<Item> items;
  // Freed (remove, deduplicate, compress), moved off the drive (relocate, archive) or held
  // (uninstall-review, manual-review).
  Bytes bytes;
  Bytes counted;            // the part of `bytes` counted under `group` (build_report)
  double confidence = 0.0;  // 0..1
  std::string reason;
  Plan plan;
  bool urgent = false;  // on a drive whose health is failing: act on it first
};

// The size on disk of every item of `finding`.
[[nodiscard]] Bytes footprint(const Finding& finding);

// One volume with a drive letter, and what scan-all could see of it.
struct DriveInfo {
  std::filesystem::path root;  // "C:\"
  placement::DriveClass drive_class = placement::DriveClass::unknown;
  health::Status health = health::Status::unknown;  // the worst of its disks
  std::string health_note;                          // the reasons when not good
  Bytes size;
  Bytes free;
  bool system = false;  // holds Windows

  std::string tree;       // "index", "scanner", or empty when no tree could be built
  bool complete = false;  // the tree covers the volume: not stopped by its budget
  Bytes scanned;          // size on disk the tree saw
  Bytes explained;        // of that, what the attribution explains
  std::optional<std::int64_t> not_visible;  // used space the tree could not see
  std::vector<std::string> notes;           // what could not be analysed, and why
};

// The classes of internal drives (every class but USB and unknown).
inline constexpr std::array<placement::DriveClass, 4> internal_classes = {
    placement::DriveClass::nvme, placement::DriveClass::sata_ssd, placement::DriveClass::nvme_qlc,
    placement::DriveClass::hdd};

// True when every disk of the drive is assessed good.
[[nodiscard]] bool healthy(const DriveInfo& drive);

// The drive that holds `path`; null when none does.
[[nodiscard]] const DriveInfo* drive_of(std::span<const DriveInfo> drives,
                                        const std::filesystem::path& path);

// placement::best_target among the drives of `classes`: another healthy drive than the one of
// `source` that keeps 10 % free after taking `needed`, by drive class and then free space. Null
// when none qualifies.
[[nodiscard]] const DriveInfo* best_target(std::span<const DriveInfo> drives,
                                           const std::filesystem::path& source, Bytes needed,
                                           std::span<const placement::DriveClass> classes);

// True for the folders of a volume that hold the system or programs: Windows, Program Files,
// Program Files (x86), ProgramData, Recovery, $Recycle.Bin, System Volume Information and the
// other folders Windows keeps in a volume root, and everything inside them.
[[nodiscard]] bool system_location(const std::filesystem::path& path);

// "1.5 GiB"
[[nodiscard]] std::string size_text(Bytes bytes);

}  // namespace myclean::scanall
