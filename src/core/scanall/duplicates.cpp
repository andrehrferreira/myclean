#include "core/scanall/duplicates.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/clean/patterns.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/scanall/findings.hpp"
#include "core/space/duplicates.hpp"
#include "core/space/scanner.hpp"
#include "core/space/text.hpp"

namespace myclean::scanall {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view budget_message = "the hash budget is spent";

// Passes hashes through while they fit in the budget. A hash that does not fit is refused, so its
// file is left out, and the smaller files that follow still get theirs; once not even a head
// hash fits, the search stops.
class BudgetHasher final : public space::IContentHasher {
 public:
  BudgetHasher(const space::IContentHasher& inner,
               std::unordered_map<std::wstring, std::uint64_t> sizes, Bytes budget,
               std::stop_source stop)
      : inner_(&inner), sizes_(std::move(sizes)), budget_(budget), stop_(std::move(stop)) {}

  [[nodiscard]] Result<space::Digest> hash(const fs::path& file,
                                           std::optional<Bytes> limit) const override {
    const auto found = sizes_.find(file.native());
    const std::uint64_t size = found == sizes_.end() ? 0 : found->second;
    const std::uint64_t bytes = limit ? std::min(limit->count(), size) : size;
    if (hashed_ + bytes > budget_.count()) {
      ++skipped_;
      if (hashed_ + space::head_hash_bytes.count() > budget_.count()) {
        stop_.request_stop();
      }
      return fail(Errc::refused, std::string(budget_message));
    }
    hashed_ += bytes;
    return inner_->hash(file, limit);
  }

  [[nodiscard]] Bytes hashed() const { return Bytes{hashed_}; }
  [[nodiscard]] std::size_t skipped() const { return skipped_; }

 private:
  const space::IContentHasher* inner_;
  std::unordered_map<std::wstring, std::uint64_t> sizes_;
  Bytes budget_;
  mutable std::stop_source stop_;
  mutable std::uint64_t hashed_ = 0;
  mutable std::size_t skipped_ = 0;
};

bool comparable(const space::FileItem& file, Bytes min_size) {
  return !file.link && !file.placeholder && !file.extra_link && file.size.count() > 0 &&
         file.size >= min_size;
}

// The size of every file that has the size of another, by path: what the hasher may read.
std::unordered_map<std::wstring, std::uint64_t> shared_sizes(
    std::span<const space::ScanResult> trees, Bytes min_size) {
  std::unordered_map<std::uint64_t, std::uint32_t> counts;
  for (const space::ScanResult& tree : trees) {
    for (const space::Folder& folder : tree.folders) {
      for (const space::FileItem& file : folder.files) {
        if (comparable(file, min_size)) {
          ++counts[file.size.count()];
        }
      }
    }
  }
  std::unordered_map<std::wstring, std::uint64_t> sizes;
  for (const space::ScanResult& tree : trees) {
    for (const space::Folder& folder : tree.folders) {
      for (const space::FileItem& file : folder.files) {
        if (comparable(file, min_size) && counts[file.size.count()] > 1) {
          sizes.emplace(space::path_of(folder, file).native(), file.size.count());
        }
      }
    }
  }
  return sizes;
}

bool inside_any(std::span<const fs::path> folders, const fs::path& path) {
  return std::ranges::any_of(
      folders, [&path](const fs::path& folder) { return clean::within(path, folder); });
}

// Higher is kept first: healthy drive, preferred folder, not transient, newest.
using Rank = std::tuple<bool, bool, bool, WallTime>;

Rank rank(const space::DuplicateFile& file, std::span<const DriveInfo> drives,
          const DuplicateOptions& options) {
  const DriveInfo* drive = drive_of(drives, file.path);
  return {drive != nullptr && healthy(*drive), inside_any(options.preferred, file.path),
          !inside_any(options.transient, file.path), file.modified};
}

void add_group(const space::DuplicateGroup& group, std::span<const DriveInfo> drives,
               const DuplicateOptions& options, std::vector<Finding>& findings) {
  const auto kept = std::ranges::min_element(
      group.files, [&](const space::DuplicateFile& left, const space::DuplicateFile& right) {
        return rank(left, drives, options) > rank(right, drives, options);
      });
  const std::string reason = std::format(
      "{} identical copies of {} (SHA-256 {}); keep {}{}", group.files.size(),
      size_text(group.size), space::to_hex(group.digest).substr(0, 16), space::utf8(kept->path),
      inside_any(options.preferred, kept->path) ? ", in a library or project" : "");
  std::map<std::wstring, Finding> by_drive;
  for (const space::DuplicateFile& file : group.files) {
    if (&file == &*kept) {
      continue;
    }
    const std::wstring root = placement::drive_root(file.path);
    auto [entry, added] = by_drive.try_emplace(root);
    Finding& finding = entry->second;
    if (added) {
      finding = Finding{.group = ActionGroup::deduplicate,
                        .source = "duplicates",
                        .drive = fs::path{root},
                        .path = file.path,
                        .confidence = 0.9,
                        .reason = reason,
                        .plan = Plan{.kind = PlanKind::quarantine_items,
                                     .params = {{"keep", space::utf8(kept->path)},
                                                {"sha256", space::to_hex(group.digest)}}}};
    }
    finding.items.push_back(Item{.path = file.path, .size = file.size_on_disk});
    finding.bytes += file.size_on_disk;
  }
  for (auto& [root, finding] : by_drive) {
    findings.push_back(std::move(finding));
  }
}

}  // namespace

DuplicateRun find_duplicates(std::span<const space::ScanResult> trees,
                             std::span<const DriveInfo> drives, const space::IContentHasher& hasher,
                             const DuplicateOptions& options, const std::stop_token& stop) {
  std::stop_source budget_stop;
  const std::stop_callback forward(stop, [&budget_stop] { budget_stop.request_stop(); });
  const BudgetHasher budgeted(hasher, shared_sizes(trees, options.min_size), options.budget,
                              budget_stop);
  const space::DuplicateReport report = space::find_duplicates(
      trees, budgeted, {.min_size = options.min_size}, budget_stop.get_token());

  DuplicateRun run{.groups = report.groups.size(),
                   .hashed = budgeted.hashed(),
                   .skipped = budgeted.skipped(),
                   .cancelled = stop.stop_requested()};
  run.errors = static_cast<std::size_t>(std::ranges::count_if(
      report.errors,
      [](const space::ItemError& error) { return error.error.message != budget_message; }));
  for (const space::DuplicateGroup& group : report.groups) {
    add_group(group, drives, options, run.findings);
  }
  return run;
}

}  // namespace myclean::scanall
