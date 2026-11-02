#include "core/map/disk_map.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/clean/patterns.hpp"
#include "core/clean/probes.hpp"
#include "core/map/attribution.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"

namespace myclean::map {

namespace {

using Components = std::vector<std::wstring>;

constexpr std::size_t none = std::numeric_limits<std::size_t>::max();

// The non-empty components of a path, split at '\' and '/'.
Components split(std::wstring_view text) {
  Components parts;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = std::min(text.find_first_of(L"\\/", start), text.size());
    if (end > start) {
      parts.emplace_back(text.substr(start, end - start));
    }
    start = end + 1;
  }
  return parts;
}

std::wstring_view leaf(const std::filesystem::path& path) {
  const std::wstring_view text = path.native();
  const std::size_t separator = text.find_last_of(L"\\/");
  return separator == std::wstring_view::npos ? text : text.substr(separator + 1);
}

// One expanded area location: a drive ("C:" or "?:") and the names below it.
struct Location {
  std::size_t area = 0;
  Components components;
};

// The locations of `areas` in file order; those with an unset variable are left out, those with
// a variable of several values (%STEAM_LIBRARY%) are one location per value.
std::vector<Location> expand(const std::vector<Area>& areas,
                             const clean::IEnvironment& environment) {
  std::vector<Location> locations;
  for (std::size_t index = 0; index < areas.size(); ++index) {
    for (const std::wstring& path : areas[index].paths) {
      for (const std::wstring& expanded : clean::expand_all(path, environment)) {
        Components components = split(expanded);
        if (!components.empty() && components.front().size() == 2 &&
            components.front()[1] == L':') {
          locations.push_back(Location{.area = index, .components = std::move(components)});
        }
      }
    }
  }
  return locations;
}

bool lesser_path(const std::filesystem::path& left, const std::filesystem::path& right) {
  return left.native() < right.native();
}

class Builder {
 public:
  Builder(const space::ScanResult& scan, DiskMap& map, std::vector<Location> locations)
      : scan_(&scan),
        map_(&map),
        locations_(std::move(locations)),
        folder_match_(scan.folders.size(), none),
        unexplained_(scan.folders.size(), 0),
        apparent_(scan.folders.size(), 0) {}

  void walk() {
    const Components root = split(scan_->root().path.native());
    map_->volume_root = root.size() == 1;
    Frame first{.folder = 0, .depth = root.size()};
    std::size_t own = none;  // the longest location that names the root or a folder above it
    for (std::size_t index = 0; index < locations_.size(); ++index) {
      const Components& components = locations_[index].components;
      const std::size_t common = std::min(components.size(), root.size());
      bool equal = true;
      for (std::size_t at = 0; at < common && equal; ++at) {
        equal = clean::wildcard_match(root[at], components[at]);
      }
      if (!equal) {
        continue;
      }
      if (components.size() > root.size()) {
        first.live.push_back(index);
      } else if (own == none || components.size() > locations_[own].components.size()) {
        own = index;
      }
    }
    if (own != none) {
      folder_match_[0] = add_match(locations_[own].area, scan_->root().path, false);
    }

    std::vector<Frame> stack;
    stack.push_back(std::move(first));
    while (!stack.empty()) {
      const Frame frame = std::move(stack.back());
      stack.pop_back();
      visit(frame, stack);
    }
    for (std::size_t index = scan_->folders.size(); index-- > 1;) {
      const std::size_t parent = scan_->folders[index].parent;
      unexplained_[parent] += unexplained_[index];
      apparent_[parent] += apparent_[index];
    }
  }

  void finish(const MapOptions& options) {
    sort_matches();
    DiskMap& map = *map_;
    map.scanned = scan_->root().size_on_disk;
    map.apparent = Bytes{apparent_[0]};
    map.unexplained = Bytes{unexplained_[0]};
    totals();
    // At least one byte, so that a minimum of zero lists only folders with unexplained bytes.
    const std::uint64_t minimum = std::max<std::uint64_t>(options.min_size.count(), 1);
    for (std::size_t index = 0; index < scan_->folders.size(); ++index) {
      if (unexplained_[index] < minimum) {
        continue;
      }
      const auto small = [this, minimum](std::size_t child) {
        return unexplained_[child] < minimum;
      };
      if (std::ranges::all_of(scan_->folders[index].folders, small)) {
        map.unexplained_folders.push_back(folder_node(index));
      }
    }
    std::ranges::sort(map.unexplained_folders, [](const Node& left, const Node& right) {
      return left.unexplained != right.unexplained ? left.unexplained > right.unexplained
                                                   : lesser_path(left.path, right.path);
    });
    std::ranges::sort(map.unreadable, [](const Unreadable& left, const Unreadable& right) {
      const std::size_t left_depth = split(left.path.native()).size();
      const std::size_t right_depth = split(right.path.native()).size();
      return left_depth != right_depth ? left_depth < right_depth
                                       : lesser_path(left.path, right.path);
    });
    map.tree = tree(0, 0, options);
  }

 private:
  struct Frame {
    std::size_t folder = 0;
    std::size_t depth = 0;          // path components of the folder
    std::vector<std::size_t> live;  // locations that name something below it, in file order
  };

  std::size_t add_match(std::size_t area, std::filesystem::path path, bool file) {
    map_->matches.push_back(Match{.area = area, .path = std::move(path), .file = file});
    return map_->matches.size() - 1;
  }

  // The first live location that names `name` itself, one level below the frame's folder.
  [[nodiscard]] std::size_t named(const Frame& frame, std::wstring_view name) const {
    for (const std::size_t index : frame.live) {
      const Components& components = locations_[index].components;
      if (components.size() == frame.depth + 1 &&
          clean::wildcard_match(name, components[frame.depth])) {
        return index;
      }
    }
    return none;
  }

  void visit(const Frame& frame, std::vector<Frame>& stack) {
    const space::Folder& folder = scan_->folders[frame.folder];
    const std::size_t match = folder_match_[frame.folder];
    if (folder.error) {
      map_->unreadable.push_back(
          Unreadable{.path = folder.path,
                     .error = *folder.error,
                     .match = match == none ? std::nullopt : std::optional<std::size_t>(match)});
      if (match != none) {
        ++map_->matches[match].unreadable;
      }
    }
    for (std::size_t index = 0; index < folder.files.size(); ++index) {
      if (!folder.files[index].link) {
        count_file(frame, index, match);
      }
    }
    for (const std::size_t child : folder.folders) {
      stack.push_back(enter(frame, child, match));
    }
  }

  // Adds file `index` of the frame's folder to its own match, the folder's, or the unexplained.
  void count_file(const Frame& frame, std::size_t index, std::size_t match) {
    const space::Folder& folder = scan_->folders[frame.folder];
    const space::FileItem& file = folder.files[index];
    std::size_t target = match;
    if (const std::size_t location = named(frame, file.name.native()); location != none) {
      target = add_match(locations_[location].area, space::path_of(folder, file), true);
      file_match_[{frame.folder, index}] = target;
    }
    apparent_[frame.folder] += file.size_on_disk.count();
    if (target != none) {
      map_->matches[target].apparent += file.size_on_disk;
    }
    if (file.extra_link) {
      return;
    }
    if (target != none) {
      map_->matches[target].size += file.size_on_disk;
    } else {
      unexplained_[frame.folder] += file.size_on_disk.count();
    }
  }

  // The frame of subfolder `child`: its match (a location that names it, else the parent's) and
  // the locations that still name something below it.
  Frame enter(const Frame& frame, std::size_t child, std::size_t match) {
    Frame next{.folder = child, .depth = frame.depth + 1};
    folder_match_[child] = match;
    if (frame.live.empty()) {
      return next;
    }
    const std::filesystem::path& path = scan_->folders[child].path;
    const std::wstring_view name = leaf(path);
    std::size_t exact = none;
    for (const std::size_t index : frame.live) {
      const Components& components = locations_[index].components;
      if (!clean::wildcard_match(name, components[frame.depth])) {
        continue;
      }
      if (components.size() > frame.depth + 1) {
        next.live.push_back(index);
      } else if (exact == none) {
        exact = index;
      }
    }
    if (exact != none) {
      folder_match_[child] = add_match(locations_[exact].area, path, false);
    }
    return next;
  }
  // Orders the matches largest first and renumbers every reference to them.
  void sort_matches() {
    std::vector<Match>& matches = map_->matches;
    std::vector<std::size_t> order(matches.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
      order[index] = index;
    }
    std::ranges::sort(order, [&matches](std::size_t left, std::size_t right) {
      return matches[left].size != matches[right].size
                 ? matches[left].size > matches[right].size
                 : lesser_path(matches[left].path, matches[right].path);
    });
    std::vector<std::size_t> renumber(matches.size());
    std::vector<Match> sorted;
    sorted.reserve(matches.size());
    for (const std::size_t index : order) {
      renumber[index] = sorted.size();
      sorted.push_back(std::move(matches[index]));
    }
    matches = std::move(sorted);
    for (std::size_t& match : folder_match_) {
      match = match == none ? none : renumber[match];
    }
    for (auto& [file, match] : file_match_) {
      match = renumber[match];
    }
    for (Unreadable& unreadable : map_->unreadable) {
      if (unreadable.match) {
        unreadable.match = renumber[*unreadable.match];
      }
    }
  }

  void totals() {
    DiskMap& map = *map_;
    std::vector<AreaTotal> areas(map.areas.size());
    for (const Match& match : map.matches) {
      AreaTotal& total = areas[match.area];
      total.area = match.area;
      total.size += match.size;
      ++total.matches;
      total.unreadable += match.unreadable;
    }
    std::vector<CategoryTotal> categories(category_count);
    for (const AreaTotal& total : areas) {
      if (total.matches == 0) {
        continue;
      }
      map.area_totals.push_back(total);
      const Area& area = map.areas[total.area];
      CategoryTotal& category = categories[static_cast<std::size_t>(area.category)];
      category.category = area.category;
      category.size += total.size;
      map.explained += total.size;
      if (reclaimable(area.reclaim.kind)) {
        category.reclaimable += total.size;
        map.reclaimable += total.size;
      } else if (area.reclaim.kind == ReclaimKind::manual_review) {
        map.review += total.size;
      }
    }
    std::ranges::stable_sort(map.area_totals, std::ranges::greater{}, &AreaTotal::size);
    for (const CategoryTotal& category : categories) {
      if (category.size.count() > 0) {
        map.categories.push_back(category);
      }
    }
    std::ranges::stable_sort(map.categories, std::ranges::greater{}, &CategoryTotal::size);
  }

  [[nodiscard]] Node folder_node(std::size_t index) const {
    const space::Folder& folder = scan_->folders[index];
    const std::size_t match = folder_match_[index];
    const std::size_t inherited = index == 0 ? none : folder_match_[folder.parent];
    return Node{.path = folder.path,
                .size = folder.size_on_disk,
                .apparent = Bytes{apparent_[index]},
                .unexplained = Bytes{unexplained_[index]},
                .match = match == none ? std::nullopt : std::optional<std::size_t>(match),
                .matched_here = match != none && match != inherited};
  }

  // NOLINTNEXTLINE(misc-no-recursion): the depth is bounded by options.depth
  [[nodiscard]] Node tree(std::size_t index, int level, const MapOptions& options) const {
    Node node = folder_node(index);
    if (level >= options.depth) {
      return node;
    }
    const space::Folder& folder = scan_->folders[index];
    for (const std::size_t child : folder.folders) {
      if (scan_->folders[child].size_on_disk >= options.min_size) {
        node.children.push_back(tree(child, level + 1, options));
      }
    }
    for (std::size_t at = 0; at < folder.files.size(); ++at) {
      const space::FileItem& file = folder.files[at];
      if (file.link || file.extra_link || file.size_on_disk < options.min_size) {
        continue;
      }
      const auto own = file_match_.find({index, at});
      const std::size_t match = own != file_match_.end() ? own->second : node.match.value_or(none);
      node.children.push_back(
          Node{.path = space::path_of(folder, file),
               .file = true,
               .size = file.size_on_disk,
               .apparent = file.size_on_disk,
               .unexplained = match == none ? file.size_on_disk : Bytes{},
               .match = match == none ? std::nullopt : std::optional<std::size_t>(match),
               .matched_here = own != file_match_.end()});
    }
    std::ranges::stable_sort(node.children, std::ranges::greater{}, &Node::size);
    return node;
  }

  const space::ScanResult* scan_;
  DiskMap* map_;
  std::vector<Location> locations_;
  std::vector<std::size_t> folder_match_;   // per folder: its match, own or inherited
  std::vector<std::uint64_t> unexplained_;  // per folder: own files, then the subtree
  std::vector<std::uint64_t> apparent_;     // per folder: own files, then the subtree
  std::map<std::pair<std::size_t, std::size_t>, std::size_t> file_match_;  // (folder, file)
};

}  // namespace

Result<DiskMap> build_map(const space::ScanResult& scan, std::vector<Area> areas,
                          const clean::IEnvironment& environment, std::optional<VolumeSpace> volume,
                          const MapOptions& options) {
  if (options.depth < 0) {
    return fail(Errc::invalid_argument, "map depth must not be negative");
  }
  DiskMap map{.root = scan.root().path,
              .cancelled = scan.cancelled,
              .options = options,
              .areas = std::move(areas),
              .volume = volume};
  Builder builder(scan, map, expand(map.areas, environment));
  builder.walk();
  builder.finish(options);
  if (map.volume_root && volume) {
    const std::uint64_t used =
        volume->total > volume->free ? volume->total.count() - volume->free.count() : 0;
    map.not_visible =
        static_cast<std::int64_t>(used) - static_cast<std::int64_t>(map.scanned.count());
  }
  return map;
}

}  // namespace myclean::map
