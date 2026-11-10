#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "core/clean/probes.hpp"
#include "core/clean/workspaces.hpp"
#include "core/map/attribution.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"

// The Dev Drive advisor (placement spec, "Dev Drive Advisor"): explains what a Dev Drive is,
// estimates which active repositories and package caches would move onto one and their size, and
// guides the owner through creating one and moving them. Advice only: nothing here creates,
// formats, shrinks or resizes a volume or partition, and nothing is moved.
namespace myclean::devdrive {

// Whether this Windows offers Dev Drive. Microsoft Learn ("Set up a Dev Drive on Windows 11"):
// Windows 11 build 22621.2338 or later.
struct Support {
  std::uint32_t build = 0;     // CurrentBuildNumber
  std::uint32_t revision = 0;  // UBR, the update revision
};

[[nodiscard]] bool supported(const Support& support);

// What one fixed volume with a drive letter is.
struct VolumeState {
  std::filesystem::path root;  // "E:\"
  std::string file_system;     // "NTFS", "ReFS"
  bool dev_drive = false;      // a developer volume (PERSISTENT_VOLUME_STATE_DEV_VOLUME)
  bool trusted = false;        // trusted by the owner: Defender scans it in performance mode
  std::string note;            // why the Dev Drive state could not be read; empty when it was
};

// Reads the Windows version and the volumes. Implemented by src/platform/devdrive; read-only, it
// has no way to change a volume.
class IDevDriveProbe {
 public:
  virtual ~IDevDriveProbe() = default;
  IDevDriveProbe(const IDevDriveProbe&) = delete;
  IDevDriveProbe& operator=(const IDevDriveProbe&) = delete;
  IDevDriveProbe(IDevDriveProbe&&) = delete;
  IDevDriveProbe& operator=(IDevDriveProbe&&) = delete;

  [[nodiscard]] virtual Result<Support> support() const = 0;
  // Every fixed volume with a drive letter, in letter order.
  [[nodiscard]] virtual Result<std::vector<VolumeState>> volumes() const = 0;

 protected:
  IDevDriveProbe() = default;
};

// A git repository (a folder holding ".git") changed within the active span.
struct Repository {
  std::filesystem::path path;
  Bytes size;  // size on disk of everything in it
  WallTime changed;
};

// A package cache a knowledge pack names (attribution category dev-caches).
struct PackageCache {
  std::string area;  // "<pack>/<case>"
  std::string label;
  std::filesystem::path path;
  Bytes size;
  // The cache relocation MyClean runs through the tool's own setting (placement::CacheKind text
  // form, "npm"), as an approved plan; empty when MyClean does not move it.
  std::string relocation;
  std::string note;  // the pack's note on reclaiming it
};

inline constexpr std::chrono::days active_span{30};

// The repositories below the workspace roots, outermost only: folders holding ".git", at most
// max_depth levels below a root, never entering a skipped name, a link or a cloud placeholder.
// Kept when something in them changed within `active` of `now`. Stops early on `stop`.
[[nodiscard]] std::vector<Repository> active_repositories(const clean::IFileTree& tree,
                                                          const clean::Workspaces& workspaces,
                                                          WallTime now,
                                                          std::chrono::days active = active_span,
                                                          const std::stop_token& stop = {});

// The folders of the dev-caches areas of `areas` that exist for the owner (`environment`) and
// hold something, each once, largest first.
[[nodiscard]] std::vector<PackageCache> package_caches(const clean::IFileTree& tree,
                                                       std::span<const map::Area> areas,
                                                       const clean::IEnvironment& environment);

// Windows' smallest Dev Drive (Microsoft Learn: at least 50 GB).
inline constexpr Bytes minimum_size{std::uint64_t{50} * 1000 * 1000 * 1000};

struct Inputs {
  Support support;
  std::vector<VolumeState> volumes;
  std::vector<placement::TargetVolume> targets;  // health, class and free space of each volume
  std::vector<Repository> repositories;
  std::vector<PackageCache> caches;
};

struct Advice {
  bool supported = false;
  Support support;
  std::vector<VolumeState> dev_drives;   // the Dev Drives present
  std::vector<Repository> repositories;  // to move: not on a Dev Drive yet
  std::vector<PackageCache> caches;      // to move: not on a Dev Drive yet
  Bytes total;                           // of what would move
  // MyClean's sizing: what would move plus half again to grow, rounded up to 10 GB, and never
  // below minimum_size.
  Bytes recommended_size;
  // Where a new Dev Drive (a dynamically expanding VHDX) would go: the healthy NVMe or SATA SSD
  // volume, not a Dev Drive itself, with the most free space that keeps 10 % free after taking
  // recommended_size; nullopt when none qualifies.
  std::optional<placement::TargetVolume> host;
  std::string explanation;         // what a Dev Drive is
  std::vector<std::string> steps;  // what the owner does, in order
  std::vector<std::string> notes;  // what could not be judged
};

[[nodiscard]] Advice advise(const Inputs& inputs);

// {"supported", "build", "dev_drives": [{"volume", "file_system", "trusted", "note"}],
//  "explanation", "repositories": [{"path", "bytes", "changed"}], "caches": [{"area", "label",
//  "path", "bytes", "relocation", "note"}], "total_bytes", "recommended_bytes", "host": null |
//  {"volume", "free", "drive_class"}, "steps": [string], "notes": [string], "sources": [string]}
// Paths UTF-8 text, times Unix seconds, sizes bytes; "build" is "<build>.<revision>".
void to_json(nlohmann::json& json, const Advice& advice);

}  // namespace myclean::devdrive
