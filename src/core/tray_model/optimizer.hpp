#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/tray_model/cleaning.hpp"

// The tray's reading of the optimizer methods for the Placement, Optimize and Startup screens
// (contracts in core/ipc/plans.hpp and core/ipc/resident.hpp). Every change these screens offer
// is a proposal that waits for the owner's approval in Cleanup.
namespace myclean::tray_model {

// The state of a background analysis (space.*): ready, scanning (the previous result, if any,
// is shown) or failed.
struct AnalysisState {
  std::string state;
  std::optional<WallTime> scanned_at;
  std::string error;
};

// ---- Placement --------------------------------------------------------------------------------

// One setting of system_space.status. When `error` is set the others are empty.
struct SystemSettingRow {
  std::string kind;  // hibernation, page_file, shadow_storage
  std::string current;
  std::optional<Bytes> cost;
  std::optional<std::string> recommended;  // nullopt: it needs no change
  bool restart_required = false;
  std::string error;
};

[[nodiscard]] Result<std::vector<SystemSettingRow>> parse_system_space(const nlohmann::json& reply);
// "Hibernation", "Page file", "Restore points (C: shadow storage)".
[[nodiscard]] std::string_view setting_name(std::string_view kind);

// The caches plans.propose_optimization relocates (placement::CacheKind text forms).
inline constexpr std::array<std::string_view, 9> relocatable_caches = {
    "temp", "gradle", "cargo", "npm", "nuget", "pip", "go_modules", "go_build", "unreal_ddc"};

// A relocation: a cache MyClean can move, or another relocation plan (a knowledge pack's recipe;
// `cache` empty), with its newest plan (null when there is none).
struct RelocationRow {
  std::string cache;
  const PlanRow* plan = nullptr;
};
// Every relocatable cache with its newest relocate plan, then the other relocate plans; `plans` is
// newest first, as plans.list answers.
[[nodiscard]] std::vector<RelocationRow> relocations(const std::vector<PlanRow>& plans);

// ---- Optimize ---------------------------------------------------------------------------------

// The changes whose effect MyClean measures (kinds system_setting and compact: system settings,
// startup items, index exclusions, virtual-disk compaction) once they were applied, newest first.
[[nodiscard]] std::vector<const PlanRow*> applied_optimizations(const std::vector<PlanRow>& plans);
// "better", "measured again later", "could not be measured", "not measured yet".
[[nodiscard]] std::string_view verification_text(std::string_view verification);

// space.indexed_folders.
struct IndexedFolderRow {
  std::int64_t id = 0;  // the candidate id plans.propose_index_exclusion takes
  std::string path;
  std::string category;
  std::string label;
};
struct IndexedFolders {
  AnalysisState analysis;
  std::vector<IndexedFolderRow> folders;
};
[[nodiscard]] Result<IndexedFolders> parse_indexed_folders(const nlohmann::json& reply);

// space.vhdx.
struct VirtualDiskRow {
  std::int64_t id = 0;  // the candidate id plans.propose_compact takes
  std::string path;
  std::string label;
  std::string distribution;
  Bytes size;
  Bytes size_on_disk;
  bool in_use = false;
};
struct VirtualDisks {
  AnalysisState analysis;
  std::vector<VirtualDiskRow> disks;
  Bytes total;
  std::vector<std::string> notes;
};
[[nodiscard]] Result<VirtualDisks> parse_vhdx(const nlohmann::json& reply);

// space.dev_drive (advice only).
struct DevDriveAdvice {
  AnalysisState analysis;
  bool supported = false;
  std::string build;
  std::vector<std::string> dev_drives;  // "D: (ReFS, trusted)"
  std::string explanation;
  std::int64_t repositories = 0;
  std::int64_t caches = 0;
  Bytes total;
  Bytes recommended;
  std::string host;  // "D: (NVMe, 1.2 TB free)"; empty when no drive qualifies
  std::vector<std::string> steps;
  std::vector<std::string> notes;
};
[[nodiscard]] Result<DevDriveAdvice> parse_dev_drive(const nlohmann::json& reply);

// resident.costs: the 30-day cost of each resident program.
struct ResidentCostRow {
  std::string image;
  double hours_running = 0.0;
  double cpu_seconds = 0.0;
  Bytes ram_average;
  Bytes ram_peak;
  Bytes disk;
  Bytes network;
};
[[nodiscard]] Result<std::vector<ResidentCostRow>> parse_resident_costs(
    const nlohmann::json& reply);

// ---- Startup ----------------------------------------------------------------------------------

// The impact of a startup item: what its program used in the first 2 minutes of the latest logons
// (source "myclean"), or the boot delay Windows attributed to it (source "diagnostics").
struct StartupImpact {
  std::string source;
  double cpu_seconds = 0.0;
  Bytes io_bytes;
  std::int64_t logons = 0;
  double delay_seconds = 0.0;
};

// One item of startup.list.
struct StartupItemRow {
  std::int64_t id = 0;  // its number in that answer
  std::string key;      // the item's stable id; a proposal names it by the id of a fresh answer
  std::string source;   // run, run32, runonce, runonce32, folder, task, service
  std::string scope;    // user, machine
  std::string name;
  std::string display_name;
  std::string command;
  std::optional<bool> enabled;  // nullopt: a RunOnce item, which cannot change
  std::string signer;
  std::optional<StartupImpact> impact;
};
[[nodiscard]] Result<std::vector<StartupItemRow>> parse_startup_list(const nlohmann::json& reply);

// "Run key", "Run key (32-bit)", "RunOnce", "Startup folder", "Scheduled task", "Service".
[[nodiscard]] std::string_view startup_source_text(std::string_view source);
// "1.2 s CPU, 35.0 MB of disk in the first 2 minutes (median of 5 logons)", "delays the boot by
// 3.4 s (Windows)", or "not measured yet".
[[nodiscard]] std::string impact_text(const std::optional<StartupImpact>& impact);

// The keys of the ticked items that can change that way: the enabled ones to disable, the disabled
// ones to enable (RunOnce items never). Fails when none can.
[[nodiscard]] Result<std::vector<std::string>> startup_choice(
    const std::vector<StartupItemRow>& items, const std::set<std::string>& ticked, bool enable);

// startup.boot_times.
struct BootContributorRow {
  std::string image;
  double cpu_seconds = 0.0;
  Bytes io_bytes;
  std::string item;  // the startup item that runs it; empty when none does
};
struct StartupChangeRow {
  std::int64_t plan = 0;
  WallTime applied;
  std::vector<std::string> items;
  std::optional<double> logon_before;  // median logon seconds of the 5 boots before
  std::optional<double> logon_after;   // and of the first 5 after; nullopt until there are some
};
struct BootTimes {
  std::string log_error;  // the log could not be read now; recorded boots are still shown
  std::optional<WallTime> latest_start;
  double boot_seconds = 0.0;
  double logon_seconds = 0.0;
  std::optional<double> boot_difference;   // from the median
  std::optional<double> logon_difference;  // from the median
  std::optional<double> median_boot;
  std::optional<double> median_logon;
  std::int64_t median_boots = 0;
  std::vector<BootContributorRow> contributors;
  std::vector<StartupChangeRow> changes;
};
[[nodiscard]] Result<BootTimes> parse_boot_times(const nlohmann::json& reply);

}  // namespace myclean::tray_model
