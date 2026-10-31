#include "core/space/duplicates.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/identity_set.hpp"
#include "core/space/scanner.hpp"
#include "core/space/text.hpp"

namespace myclean::space {

namespace {

struct FileRef {
  const Folder* folder;
  const FileItem* file;
};

class Finder {
 public:
  Finder(const IContentHasher& hasher, std::stop_token stop)
      : hasher_(&hasher), stop_(std::move(stop)) {}

  // Groups the files of one size by content and records each group of two or more.
  void check(std::uint64_t size, const std::vector<FileRef>& refs) {
    std::vector<DuplicateFile> files;
    files.reserve(refs.size());
    for (const FileRef& ref : refs) {
      files.push_back(DuplicateFile{.path = path_of(*ref.folder, *ref.file),
                                    .size_on_disk = ref.file->size_on_disk,
                                    .modified = ref.file->modified,
                                    .identity = ref.file->identity});
    }
    // Up to 64 KB the head is the whole content, so one hash decides.
    if (size <= head_hash_bytes.count()) {
      for (auto& [digest, same] : split(files, std::nullopt)) {
        add(size, digest, std::move(same));
      }
      return;
    }
    for (auto& [head, same_head] : split(files, head_hash_bytes)) {
      for (auto& [digest, same] : split(same_head, std::nullopt)) {
        add(size, digest, std::move(same));
      }
    }
  }

  [[nodiscard]] bool cancelled() const { return report_.cancelled; }
  [[nodiscard]] DuplicateReport take() { return std::move(report_); }

 private:
  // The files of `files` that share a hash with another, by hash.
  std::map<Digest, std::vector<DuplicateFile>> split(std::vector<DuplicateFile>& files,
                                                     std::optional<Bytes> limit) {
    std::map<Digest, std::vector<DuplicateFile>> by_digest;
    for (DuplicateFile& file : files) {
      if (stop_.stop_requested()) {
        report_.cancelled = true;
        return {};
      }
      const Result<Digest> digest = hasher_->hash(file.path, limit);
      if (!digest) {
        report_.errors.push_back(ItemError{.path = file.path, .error = digest.error()});
        continue;
      }
      by_digest[*digest].push_back(std::move(file));
    }
    std::erase_if(by_digest, [](const auto& entry) { return entry.second.size() < 2; });
    return by_digest;
  }

  void add(std::uint64_t size, const Digest& digest, std::vector<DuplicateFile> files) {
    std::ranges::sort(files, {}, &DuplicateFile::path);
    std::uint64_t total = 0;
    std::uint64_t largest = 0;
    for (const DuplicateFile& file : files) {
      total += file.size_on_disk.count();
      largest = std::max(largest, file.size_on_disk.count());
    }
    report_.groups.push_back(DuplicateGroup{.size = Bytes{size},
                                            .digest = digest,
                                            .reclaimable = Bytes{total - largest},
                                            .files = std::move(files)});
  }

  const IContentHasher* hasher_;
  std::stop_token stop_;
  DuplicateReport report_;
};

bool is_candidate(const FileItem& file, const DuplicateOptions& options) {
  if (file.link || file.placeholder || file.extra_link || file.size.count() == 0 ||
      file.size < options.min_size) {
    return false;
  }
  return options.extensions.empty() ||
         std::ranges::find(options.extensions, lower_extension(file.name)) !=
             options.extensions.end();
}

}  // namespace

DuplicateReport find_duplicates(std::span<const ScanResult> scans, const IContentHasher& hasher,
                                const DuplicateOptions& options, const std::stop_token& stop) {
  IdentitySet seen;
  std::unordered_map<std::uint64_t, std::vector<FileRef>> by_size;
  for (const ScanResult& scan : scans) {
    for (const Folder& folder : scan.folders) {
      for (const FileItem& file : folder.files) {
        if (is_candidate(file, options) && seen.insert(file.identity)) {
          by_size[file.size.count()].push_back(FileRef{.folder = &folder, .file = &file});
        }
      }
    }
  }

  // Largest sizes first, so that a cancelled search has found the biggest savings.
  std::vector<std::uint64_t> sizes;
  for (const auto& [size, refs] : by_size) {
    if (refs.size() > 1) {
      sizes.push_back(size);
    }
  }
  std::ranges::sort(sizes, std::greater{});

  Finder finder{hasher, stop};
  for (const std::uint64_t size : sizes) {
    if (finder.cancelled()) {
      break;
    }
    finder.check(size, by_size[size]);
  }
  DuplicateReport report = finder.take();
  std::ranges::sort(report.groups, [](const DuplicateGroup& left, const DuplicateGroup& right) {
    if (left.reclaimable != right.reclaimable) {
      return left.reclaimable > right.reclaimable;
    }
    return left.files.front().path < right.files.front().path;
  });
  return report;
}

std::string to_hex(const Digest& digest) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string text;
  text.reserve(digest.size() * 2);
  for (const std::uint8_t byte : digest) {
    text.push_back(digits[byte >> 4U]);
    text.push_back(digits[byte & 0x0FU]);
  }
  return text;
}

}  // namespace myclean::space
