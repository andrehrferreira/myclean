#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "core/clean/probes.hpp"
#include "core/map/attribution.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"

// "Where did my disk go": a scan of a volume or folder explained with data/attribution.toml.
// Every byte the scan counted belongs to exactly one place: the most precise area location that
// names its file or one of the file's folders, or none (unexplained). What the scan could not
// see is reported as such, never estimated.
namespace myclean::map {

struct VolumeSpace {
  Bytes total;
  Bytes free;
};

struct MapOptions {
  Bytes min_size{1ULL << 30U};  // smallest folder or file shown in the tree and lists
  int depth = 6;                // tree levels shown below the root
};

// One folder or file an area location named.
struct Match {
  std::size_t area = 0;  // index into DiskMap::areas
  std::filesystem::path path;
  bool file = false;
  // Size on disk of what it holds, without what more precise matches inside it hold. Hard links
  // count once, in the first folder (by name, depth first) that has one.
  Bytes size;
  Bytes apparent;                // the same with every hard link counted where it is
  std::uint64_t unreadable = 0;  // folders inside it that could not be listed
};

// Everything one area explains.
struct AreaTotal {
  std::size_t area = 0;
  Bytes size;
  std::uint64_t matches = 0;
  std::uint64_t unreadable = 0;
};

struct CategoryTotal {
  Category category = Category::other;
  Bytes size;
  Bytes reclaimable;  // held by areas whose reclaim kind is reclaimable()
};

struct Node {
  std::filesystem::path path;
  bool file = false;
  Bytes size;                        // size on disk, hard links counted once
  Bytes apparent;                    // hard links counted in every folder that has one
  Bytes unexplained;                 // part of `size` no area explains
  std::optional<std::size_t> match;  // index into DiskMap::matches: the area the node is in
  bool matched_here = false;         // the match is this node itself, not an ancestor
  std::vector<Node> children;        // largest first
};

struct Unreadable {
  std::filesystem::path path;
  Error error{.code = Errc::io_error, .message = {}};
  std::optional<std::size_t> match;  // the area it lies in, when one does
};

struct DiskMap {
  std::filesystem::path root;
  bool volume_root = false;  // the scan covered a whole volume
  bool cancelled = false;    // the scan stopped early
  MapOptions options;
  std::vector<Area> areas;  // the attribution the map used

  Bytes scanned;      // size on disk of everything the scan saw
  Bytes apparent;     // the same with every hard link counted
  Bytes explained;    // held by some area
  Bytes unexplained;  // held by none: scanned - explained
  Bytes reclaimable;  // held by areas whose reclaim kind is reclaimable()
  Bytes review;       // held by manual-review areas

  std::optional<VolumeSpace> volume;
  // Used space (total - free) minus `scanned`, for a volume root with known space: what the
  // scan could not see. Negative when the volume changed during the scan.
  std::optional<std::int64_t> not_visible;

  std::vector<Match> matches;             // largest first; every match with content or errors
  std::vector<AreaTotal> area_totals;     // largest first; areas with content or errors
  std::vector<CategoryTotal> categories;  // largest first; categories with content
  std::vector<Node> unexplained_folders;  // largest first; see build_map
  std::vector<Unreadable> unreadable;     // shallowest first, then by path
  Node tree;
};

// Explains `scan` with `areas`, whose locations are expanded with `environment` (a location with
// an unset variable is skipped). `volume` is the space of the volume holding the scan root.
// The tree holds the root, then every folder and file of at least options.min_size down to
// options.depth levels. unexplained_folders are the deepest folders with at least min_size
// unexplained whose subfolders each hold less than that. Fails with invalid_argument when
// options.depth < 0.
[[nodiscard]] Result<DiskMap> build_map(const space::ScanResult& scan, std::vector<Area> areas,
                                        const clean::IEnvironment& environment,
                                        std::optional<VolumeSpace> volume,
                                        const MapOptions& options = {});

}  // namespace myclean::map
