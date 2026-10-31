#include "core/space/scanner.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/identity_set.hpp"

namespace myclean::space {

namespace {

// IsReparseTagNameSurrogate: the tag names another entry (junction, symbolic link, mount point).
constexpr std::uint32_t name_surrogate_bit = 0x20000000U;

bool is_link(const ListedEntry& entry) {
  return entry.attributes.reparse_point &&
         (entry.directory || (entry.reparse_tag & name_surrogate_bit) != 0);
}

FileItem item_of(ListedEntry& entry) {
  const bool link = is_link(entry);
  return FileItem{.name = std::move(entry.name),
                  .link = link,
                  .placeholder = !link && entry.attributes.cloud_placeholder(),
                  .reparse_tag = link ? entry.reparse_tag : 0,
                  .size = link ? Bytes{} : entry.size,
                  .size_on_disk = link ? Bytes{} : entry.size_on_disk,
                  .created = entry.created,
                  .modified = entry.modified,
                  .identity = entry.identity};
}

// State shared by the scanner threads; every member is guarded by `mutex`.
struct Shared {
  std::mutex mutex;
  std::condition_variable_any wake;
  std::deque<std::size_t> pending;  // folders not listed yet
  unsigned busy = 0;                // folders being listed now
  ScanResult result;
  ScanProgress progress;
  std::uint64_t next_report = 0;
};

// A listed folder, split into its files and links and its subfolders.
struct Listing {
  std::vector<FileItem> files;
  std::vector<std::filesystem::path> subfolders;
  std::size_t entries = 0;
  Bytes size_on_disk;
};

Listing split(std::vector<ListedEntry>& entries, const std::filesystem::path& directory) {
  Listing listing{.entries = entries.size()};
  for (ListedEntry& entry : entries) {
    if (entry.directory && !is_link(entry)) {
      listing.subfolders.push_back(directory / entry.name);
    } else {
      listing.files.push_back(item_of(entry));
      listing.size_on_disk += listing.files.back().size_on_disk;
    }
  }
  return listing;
}

// Waits for a folder to list and marks it busy; nullopt when the scan is complete or stopped.
std::optional<std::pair<std::size_t, std::filesystem::path>> next_folder(
    Shared& shared, const std::stop_token& stop) {
  std::unique_lock lock{shared.mutex};
  const bool ready = shared.wake.wait(
      lock, stop, [&shared] { return !shared.pending.empty() || shared.busy == 0; });
  if (!ready || stop.stop_requested() || shared.pending.empty()) {
    shared.wake.notify_all();
    return std::nullopt;
  }
  const std::size_t index = shared.pending.front();
  shared.pending.pop_front();
  ++shared.busy;
  return std::pair{index, shared.result.folders[index].path};
}

// Adds the listing of folder `index` to the tree.
void record(Shared& shared, std::size_t index, Result<Listing> listed, const ScanOptions& options,
            const ProgressCallback& progress) {
  const std::scoped_lock lock{shared.mutex};
  --shared.busy;
  std::vector<Folder>& folders = shared.result.folders;
  if (!listed) {
    folders[index].error = std::move(listed.error());
    ++shared.result.error_count;
  } else {
    for (std::filesystem::path& path : listed->subfolders) {
      const std::size_t child = folders.size();
      folders.push_back(Folder{.path = std::move(path), .parent = index});
      folders[index].folders.push_back(child);
      shared.pending.push_back(child);
    }
    folders[index].files = std::move(listed->files);
    ++shared.progress.folders;
    shared.progress.files += listed->entries;
    shared.progress.size_on_disk += listed->size_on_disk;
    if (progress && shared.progress.files >= shared.next_report) {
      progress(shared.progress);
      shared.next_report = shared.progress.files + options.progress_every;
    }
  }
  shared.wake.notify_all();
}

void run_worker(const IDirectoryReader& reader, Shared& shared, const ScanOptions& options,
                const std::stop_token& stop, const ProgressCallback& progress) {
  while (const auto folder = next_folder(shared, stop)) {
    const auto& [index, directory] = *folder;
    // The listing, the slow part, runs without the lock.
    Result<std::vector<ListedEntry>> entries = reader.list(directory);
    Result<Listing> listing =
        entries ? Result<Listing>{split(*entries, directory)} : tl::unexpected(entries.error());
    record(shared, index, std::move(listing), options, progress);
  }
}

// Sorts every folder's entries, marks extra hard links in a fixed (depth-first, by name) order,
// so that the same tree always yields the same totals, then adds the totals up.
void finish(ScanResult& result) {
  std::vector<Folder>& folders = result.folders;
  for (Folder& folder : folders) {
    std::ranges::sort(folder.files, {}, &FileItem::name);
    std::ranges::sort(folder.folders, {},
                      [&folders](std::size_t index) -> const auto& { return folders[index].path; });
  }

  IdentitySet counted;
  std::vector<std::size_t> stack{0};
  while (!stack.empty()) {
    Folder& folder = folders[stack.back()];
    stack.pop_back();
    for (FileItem& file : folder.files) {
      if (file.link) {
        ++result.link_count;
        continue;
      }
      if (file.placeholder) {
        ++result.placeholder_count;
      }
      file.extra_link = !counted.insert(file.identity);
    }
    stack.insert(stack.end(), folder.folders.rbegin(), folder.folders.rend());
  }

  for (std::size_t index = folders.size(); index-- > 0;) {
    Folder& folder = folders[index];
    for (const FileItem& file : folder.files) {
      if (file.link) {
        continue;
      }
      ++folder.file_count;
      if (!file.extra_link) {
        folder.own_size_on_disk += file.size_on_disk;
        folder.size += file.size;
      }
    }
    folder.size_on_disk += folder.own_size_on_disk;
    if (folder.parent != no_parent) {
      Folder& parent = folders[folder.parent];
      parent.size_on_disk += folder.size_on_disk;
      parent.size += folder.size;
      parent.file_count += folder.file_count;
      parent.folder_count += folder.folder_count + 1;
    }
  }
}

}  // namespace

Result<ScanResult> scan(const IDirectoryReader& reader, const std::filesystem::path& root,
                        const ScanOptions& options, const std::stop_token& stop,
                        const ProgressCallback& progress) {
  if (!root.is_absolute()) {
    return fail(Errc::invalid_argument, "scan root is not absolute");
  }
  if (options.workers == 0 || options.workers > 64) {
    return fail(Errc::invalid_argument, "scan workers must be 1..64");
  }

  Shared shared;
  shared.result.folders.push_back(Folder{.path = root});
  shared.pending.push_back(0);
  {
    std::vector<std::jthread> workers;
    workers.reserve(options.workers);
    for (unsigned i = 0; i < options.workers; ++i) {
      workers.emplace_back([&] { run_worker(reader, shared, options, stop, progress); });
    }
  }

  ScanResult result = std::move(shared.result);
  if (result.folders.front().error) {
    return tl::unexpected(*result.folders.front().error);
  }
  result.cancelled = !shared.pending.empty();
  finish(result);
  return result;
}

}  // namespace myclean::space
