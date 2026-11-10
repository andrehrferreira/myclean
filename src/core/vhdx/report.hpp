#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "core/clean/probes.hpp"
#include "core/map/attribution.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/units.hpp"

// The WSL and Docker virtual disk report (optimizer spec, "VHDX Size Report and Compaction"):
// every .vhdx where the knowledge packs say WSL distributions and Docker Desktop keep them, with
// its path and size. Read-only.
namespace myclean::vhdx {

// A place a virtual disk may be: a .vhdx file, or a folder holding them (up to two levels down).
struct DiskSource {
  std::filesystem::path location;
  std::string area;          // "<pack>/<case>": the attribution area or relocation recipe
  std::string label;         // what the pack calls it
  std::string distribution;  // the WSL distribution, when the source names one
};

struct VirtualDisk {
  std::filesystem::path path;
  std::string area;
  std::string label;
  std::string distribution;
  Bytes size;          // the file's size
  Bytes size_on_disk;  // what it occupies
  FileIdentity identity;
  bool in_use = false;  // a process holds it open: its distribution or Docker Desktop runs
};

// True for a name ending in ".vhdx" (any case).
[[nodiscard]] bool is_vhdx(const std::filesystem::path& path);

// The sources of the vm-disks areas of `areas` that exist for the owner (`environment`).
[[nodiscard]] std::vector<DiskSource> area_sources(const clean::IFileTree& tree,
                                                   std::span<const map::Area> areas,
                                                   const clean::IEnvironment& environment);

// Every .vhdx of `sources`, each file once (the first source naming it wins, so put the most
// specific first), largest first. Links and cloud placeholders are neither entered nor listed.
// `usage` (may be null) tells which are in use.
[[nodiscard]] std::vector<VirtualDisk> find_disks(const clean::IFileTree& tree,
                                                  std::span<const DiskSource> sources,
                                                  const clean::IUsage* usage);

// {"path", "area", "label", "distribution": string|null, "size", "size_on_disk", "in_use"}
void to_json(nlohmann::json& json, const VirtualDisk& disk);

}  // namespace myclean::vhdx
