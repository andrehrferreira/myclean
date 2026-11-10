#include "core/devdrive/advisor.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/clean/patterns.hpp"
#include "core/clean/probes.hpp"
#include "core/clean/walk.hpp"
#include "core/clean/workspaces.hpp"
#include "core/map/attribution.hpp"
#include "core/model/text.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/relocation.hpp"

namespace myclean::devdrive {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::string_view source = "https://learn.microsoft.com/windows/dev-drive/";
constexpr std::uint64_t ten_gb = std::uint64_t{10} * 1000 * 1000 * 1000;

bool enterable(const clean::Entry& entry) {
  return entry.stat.kind == os::EntryKind::directory && !entry.stat.attributes.reparse_point &&
         !entry.stat.attributes.cloud_placeholder();
}

bool same_name(const std::wstring& name, const std::wstring& other) {
  return clean::wildcard_match(name, other);  // ASCII case-insensitive, no wildcard in `other`
}

bool skipped(const std::wstring& name, const clean::Workspaces& workspaces) {
  return std::ranges::any_of(workspaces.skip, [&](const std::wstring& skip) {
    return !clean::has_wildcard(skip) && same_name(name, skip);
  });
}

std::string gb(Bytes bytes) {
  return std::format("{:.1f} GB", static_cast<double>(bytes.count()) / 1e9);
}

std::string utf8(const fs::path& path) {
  return utf8_text(path);
}

// The drive letter of `path` with its colon ("E:"); empty for a path without one.
std::string letter_of(const fs::path& path) {
  const std::string root = utf8(path.root_name());
  return root.size() == 2 && root[1] == ':' ? root : std::string{};
}

bool holds_git(const std::vector<clean::Entry>& entries) {
  return std::ranges::any_of(entries, [](const clean::Entry& entry) {
    return same_name(entry.path.filename().native(), L".git");
  });
}

// Queues the folders of `entries` a repository search enters, one level below `depth`.
void queue_folders(const std::vector<clean::Entry>& entries, int depth,
                   const clean::Workspaces& workspaces,
                   std::deque<std::pair<fs::path, int>>& pending) {
  for (const clean::Entry& entry : entries) {
    if (enterable(entry) && !skipped(entry.path.filename().native(), workspaces)) {
      pending.emplace_back(entry.path, depth + 1);
    }
  }
}

// The entries the paths of `area` name for the owner, links left out.
std::vector<clean::Entry> area_entries(const clean::IFileTree& tree, const map::Area& area,
                                       const clean::IEnvironment& environment) {
  std::vector<clean::Entry> entries;
  for (const std::wstring& pattern : area.paths) {
    for (const std::wstring& expanded : clean::expand_all(pattern, environment)) {
      for (clean::Entry& entry : clean::resolve(tree, fs::path{expanded}).entries) {
        if (!entry.stat.attributes.reparse_point) {
          entries.push_back(std::move(entry));
        }
      }
    }
  }
  return entries;
}

bool on_dev_drive(const fs::path& path, std::span<const VolumeState> dev_drives) {
  return std::ranges::any_of(
      dev_drives, [&](const VolumeState& volume) { return clean::within(path, volume.root); });
}

std::int64_t unix_seconds(WallTime time) {
  return std::chrono::floor<std::chrono::seconds>(time.time_since_epoch()).count();
}

std::optional<placement::TargetVolume> host_for(std::span<const placement::TargetVolume> targets,
                                                std::span<const VolumeState> dev_drives,
                                                Bytes size) {
  std::optional<placement::TargetVolume> best;
  for (const placement::TargetVolume& target : targets) {
    const bool solid_state = target.drive_class == placement::DriveClass::nvme ||
                             target.drive_class == placement::DriveClass::sata_ssd;
    if (!target.healthy || !solid_state || on_dev_drive(target.root, dev_drives) ||
        target.free < size || (target.free.count() - size.count()) * 10 < target.size.count()) {
      continue;
    }
    if (!best || target.free > best->free) {
      best = target;
    }
  }
  return best;
}

std::string labels_of(std::span<const PackageCache> caches) {
  std::string text;
  for (const PackageCache& cache : caches) {
    text += (text.empty() ? "" : ", ") + cache.label + " (" + gb(cache.size) + ")";
  }
  return text;
}

// The steps that move what would go onto the Dev Drive `where` ("the Dev Drive" or "E:").
void moving_steps(Advice& advice, const std::string& where) {
  std::vector<PackageCache> relocatable;
  std::vector<PackageCache> by_hand;
  for (const PackageCache& cache : advice.caches) {
    (cache.relocation.empty() ? by_hand : relocatable).push_back(cache);
  }
  if (!relocatable.empty()) {
    advice.steps.emplace_back(
        "Move the package caches MyClean knows how to move: " + labels_of(relocatable) +
        ". Choose " + where +
        " as the target in the MyClean tray's Relocate view (or ask an AI assistant to propose "
        "it: propose_optimization with kind relocate, the cache and the drive letter); each "
        "move changes only the tool's own cache setting, waits for your approval and can be "
        "undone.");
  }
  for (const PackageCache& cache : by_hand) {
    advice.steps.emplace_back("Point " + cache.label + " (" + utf8(cache.path) + ", " +
                              gb(cache.size) + ") at a folder on " + where +
                              " with the tool's own cache setting" +
                              (cache.note.empty() ? "." : ": " + cache.note));
  }
  if (!advice.repositories.empty()) {
    Bytes size{};
    for (const Repository& repository : advice.repositories) {
      size += repository.size;
    }
    advice.steps.emplace_back(std::format(
        "Move the {} active repositories ({}) onto {}: close the editors, terminals and builds "
        "that use one, then clone it again there (or move its folder) and reopen it from the "
        "new place.",
        advice.repositories.size(), gb(size), where));
  }
  advice.steps.emplace_back(
      "Keep Windows, programs and documents where they are: a Dev Drive is meant for source "
      "code, package caches and build output.");
}

}  // namespace

bool supported(const Support& support) {
  constexpr std::uint32_t first_build = 22621;
  constexpr std::uint32_t first_revision = 2338;
  return support.build > first_build ||
         (support.build == first_build && support.revision >= first_revision);
}

std::vector<Repository> active_repositories(const clean::IFileTree& tree,
                                            const clean::Workspaces& workspaces, WallTime now,
                                            std::chrono::days active, const std::stop_token& stop) {
  std::vector<Repository> found;
  std::set<fs::path> seen;
  std::deque<std::pair<fs::path, int>> pending;
  for (const fs::path& root : workspaces.roots) {
    pending.emplace_back(root, 0);
  }
  while (!pending.empty() && !stop.stop_requested()) {
    auto [folder, depth] = std::move(pending.front());
    pending.pop_front();
    const Result<std::vector<clean::Entry>> listed = tree.list(folder);
    if (!listed) {
      continue;
    }
    if (holds_git(*listed)) {
      if (!seen.insert(folder).second) {
        continue;
      }
      const Result<clean::Entry> self = tree.stat(folder);
      if (!self) {
        continue;
      }
      const clean::Measure measured = clean::measure(tree, *self);
      if (measured.newest >= now - active) {
        found.push_back(
            Repository{.path = folder, .size = measured.size, .changed = measured.newest});
      }
      continue;  // a repository inside a repository moves with it
    }
    if (depth < workspaces.max_depth) {
      queue_folders(*listed, depth, workspaces, pending);
    }
  }
  std::ranges::sort(found, [](const Repository& left, const Repository& right) {
    return left.size != right.size ? left.size > right.size : left.path < right.path;
  });
  return found;
}

std::vector<PackageCache> package_caches(const clean::IFileTree& tree,
                                         std::span<const map::Area> areas,
                                         const clean::IEnvironment& environment) {
  std::vector<PackageCache> found;
  std::set<fs::path> seen;
  for (const map::Area& area : areas) {
    if (area.category != map::Category::dev_caches) {
      continue;
    }
    const std::string relocation = area.reclaim.kind == map::ReclaimKind::relocate &&
                                           placement::parse_cache_kind(area.reclaim.ref)
                                       ? area.reclaim.ref
                                       : std::string{};
    for (const clean::Entry& entry : area_entries(tree, area, environment)) {
      if (!seen.insert(entry.path).second) {
        continue;
      }
      const clean::Measure measured = clean::measure(tree, entry);
      if (measured.size.count() > 0) {
        found.push_back(PackageCache{.area = area.id,
                                     .label = area.label,
                                     .path = entry.path,
                                     .size = measured.size,
                                     .relocation = relocation,
                                     .note = area.reclaim.note});
      }
    }
  }
  std::ranges::sort(found, [](const PackageCache& left, const PackageCache& right) {
    return left.size != right.size ? left.size > right.size : left.path < right.path;
  });
  return found;
}

Advice advise(const Inputs& inputs) {
  Advice advice{.supported = supported(inputs.support), .support = inputs.support};
  advice.explanation =
      "A Dev Drive is a volume formatted with ReFS and marked for development: Microsoft "
      "Defender scans a trusted Dev Drive in performance mode (it defers its scan until after a "
      "file is opened), and ReFS copies files with block cloning. Builds, package "
      "restores and git, which touch many small files, run faster there. It holds source code, "
      "package caches and build output, not Windows or programs. It can be a partition or a "
      "virtual disk file (VHDX) on an existing drive; Windows requires at least 50 GB.";
  for (const VolumeState& volume : inputs.volumes) {
    if (volume.dev_drive) {
      advice.dev_drives.push_back(volume);
    }
    if (!volume.note.empty()) {
      advice.notes.emplace_back(utf8(volume.root) + ": " + volume.note);
    }
  }
  for (const Repository& repository : inputs.repositories) {
    if (!on_dev_drive(repository.path, advice.dev_drives)) {
      advice.repositories.push_back(repository);
      advice.total += repository.size;
    }
  }
  for (const PackageCache& cache : inputs.caches) {
    if (!on_dev_drive(cache.path, advice.dev_drives)) {
      advice.caches.push_back(cache);
      advice.total += cache.size;
    }
  }
  const std::uint64_t wanted = advice.total.count() + (advice.total.count() / 2);
  advice.recommended_size = std::max(minimum_size, Bytes{(wanted + ten_gb - 1) / ten_gb * ten_gb});

  if (!advice.supported) {
    advice.notes.emplace_back(std::format(
        "This Windows (build {}.{}) has no Dev Drive: Windows 11 build 22621.2338 or later adds "
        "it through Windows Update.",
        inputs.support.build, inputs.support.revision));
    return advice;
  }
  if (!advice.dev_drives.empty()) {
    const VolumeState& drive = advice.dev_drives.front();
    const std::string letter = letter_of(drive.root);
    if (!drive.trusted) {
      advice.steps.emplace_back("Dev Drive " + letter +
                                " is not trusted, so Defender scans it the usual way: run `fsutil "
                                "devdrv trust " +
                                letter +
                                "` in an administrator terminal, then `fsutil devdrv query " +
                                letter + "` to check.");
    }
    moving_steps(advice, letter);
    return advice;
  }
  advice.host = host_for(inputs.targets, advice.dev_drives, advice.recommended_size);
  advice.steps.emplace_back(
      "Open Settings > System > Storage > Advanced storage settings > Disks & volumes and select "
      "Create Dev Drive (Windows asks for administrator approval).");
  if (advice.host) {
    advice.steps.emplace_back(
        "Choose Create new VHD: location " + utf8(advice.host->root) + " (" +
        gb(advice.host->free) + " free), size " + gb(advice.recommended_size) +
        ", format VHDX, Dynamically expanding. A virtual disk is a file that grows as it fills; "
        "no partition changes.");
  } else {
    advice.steps.emplace_back(
        "Choose Create new VHD on a healthy SSD with at least " + gb(advice.recommended_size) +
        " free (no partition changes). No drive here has that much room while keeping 10 % "
        "free; Resize an existing volume and Unallocated space change partitions, so back up "
        "first.");
  }
  advice.steps.emplace_back(
      "Name it, pick a drive letter and select Format: Windows formats it as ReFS and marks it "
      "as a trusted Dev Drive.");
  advice.steps.emplace_back(
      "Check it in a terminal: `fsutil devdrv query <letter>:` says whether it is a trusted "
      "developer volume; Microsoft Defender then scans it in performance mode.");
  moving_steps(advice, "the Dev Drive");
  return advice;
}

void to_json(Json& json, const Advice& advice) {
  Json dev_drives = Json::array();
  for (const VolumeState& volume : advice.dev_drives) {
    dev_drives.push_back({{"volume", letter_of(volume.root)},
                          {"file_system", volume.file_system},
                          {"trusted", volume.trusted},
                          {"note", volume.note}});
  }
  Json repositories = Json::array();
  for (const Repository& repository : advice.repositories) {
    repositories.push_back({{"path", utf8(repository.path)},
                            {"bytes", repository.size.count()},
                            {"changed", unix_seconds(repository.changed)}});
  }
  Json caches = Json::array();
  for (const PackageCache& cache : advice.caches) {
    caches.push_back({{"area", cache.area},
                      {"label", cache.label},
                      {"path", utf8(cache.path)},
                      {"bytes", cache.size.count()},
                      {"relocation", cache.relocation},
                      {"note", cache.note}});
  }
  Json host = nullptr;
  if (advice.host) {
    host = {{"volume", letter_of(advice.host->root)},
            {"free", advice.host->free.count()},
            {"drive_class", placement::to_string(advice.host->drive_class)}};
  }
  json = {{"supported", advice.supported},
          {"build", std::format("{}.{}", advice.support.build, advice.support.revision)},
          {"dev_drives", std::move(dev_drives)},
          {"explanation", advice.explanation},
          {"repositories", std::move(repositories)},
          {"caches", std::move(caches)},
          {"total_bytes", advice.total.count()},
          {"recommended_bytes", advice.recommended_size.count()},
          {"host", std::move(host)},
          {"steps", advice.steps},
          {"notes", advice.notes},
          {"sources", Json::array({std::string(source)})}};
}

}  // namespace myclean::devdrive
