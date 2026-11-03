#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/clean/probes.hpp"
#include "core/map/attribution.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/settings.hpp"
#include "core/scanall/findings.hpp"

// Owner decisions (owner-decisions spec): what only the owner can decide, as the service lists it
// for the tray's one-click actions. The cold folders come from find_cold(); this adds the archive
// folder rule they share with the archive action, the drives the service knows, and the installed
// programs the knowledge packs mark for uninstall review. Read-only.
namespace myclean::scanall {

// "<root>Archive\<letter>\<path below its drive root>": where an archived folder goes on the HDD
// whose root is `root` ("L:\Archive\C\Users\owner\Old" for "C:\Users\owner\Old"). Empty when
// `source` has no drive letter.
[[nodiscard]] std::filesystem::path archive_folder(const std::filesystem::path& source,
                                                   const std::filesystem::path& root);

// The archive folder of `source` (`size` bytes) on the healthy HDD with the most room that keeps
// 10 % free after taking it (best_target over the HDDs); nullopt when no HDD qualifies.
[[nodiscard]] std::optional<std::filesystem::path> archive_target(
    std::span<const DriveInfo> drives, const std::filesystem::path& source, Bytes size);

// The volumes placement reads as the scan's drives: a healthy volume has health good, any other
// unknown (with its health note).
[[nodiscard]] std::vector<DriveInfo> drive_infos(std::span<const placement::TargetVolume> volumes);

// The Uninstall keys of the installed programs, as placement::IRegistry names them: the 64-bit
// and 32-bit views of HKLM and the owner's HKCU.
inline constexpr std::array<std::string_view, 3> uninstall_roots = {
    R"(HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)",
    R"(HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall)",
    R"(HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)"};

// One installed program's Uninstall entry.
struct UninstallEntry {
  std::string key;   // the full key: "<one of uninstall_roots>\<subkey>"
  std::string name;  // DisplayName
  std::string publisher;
  std::string version;             // DisplayVersion
  std::filesystem::path location;  // InstallLocation; empty when the entry has none
  std::string uninstall;           // UninstallString: the program's own interactive uninstaller
  Bytes estimated;                 // EstimatedSize (stored in KiB); zero when absent

  friend bool operator==(const UninstallEntry&, const UninstallEntry&) = default;
};

// The entry at `key` (one of uninstall_roots' subkeys); nullopt when the key, its DisplayName or
// its UninstallString is missing (the program is not installed, or cannot be uninstalled).
[[nodiscard]] Result<std::optional<UninstallEntry>> uninstall_entry(
    const placement::IRegistry& registry, std::string_view key);

// Every entry of uninstall_roots that a person can uninstall: with a DisplayName and an
// UninstallString, not a SystemComponent and not an update of another entry (ParentKeyName).
[[nodiscard]] Result<std::vector<UninstallEntry>> uninstall_entries(
    const placement::IRegistry& registry);

// An installed program that lies in an area the packs mark for uninstall review.
struct ProgramFinding {
  UninstallEntry program;
  std::string area;  // the area's label
  std::string note;  // the area's reclaim note
};

// The programs of `programs` whose InstallLocation is at or below a location of an area of
// `areas` whose reclaim kind is uninstall (locations expanded with `environment`; '*' and '?'
// match within one component, so "?:\Epic Games\UE_*" holds "D:\Epic Games\UE_5.3\Engine").
// A program without an InstallLocation cannot be placed and is left out.
[[nodiscard]] std::vector<ProgramFinding> uninstall_review(std::span<const UninstallEntry> programs,
                                                           std::span<const map::Area> areas,
                                                           const clean::IEnvironment& environment);

}  // namespace myclean::scanall
