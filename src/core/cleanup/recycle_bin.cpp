#include "core/cleanup/recycle_bin.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <ratio>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/cleanup/handler_cleanup.hpp"
#include "core/cleanup/support.hpp"
#include "core/model/action.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/safety/paths.hpp"

namespace myclean::cleanup {

namespace {

constexpr std::size_t header_size = 24;     // version, size, deletion time
constexpr std::size_t v1_path_units = 260;  // MAX_PATH
constexpr std::size_t v1_size = header_size + (2 * v1_path_units);
constexpr std::size_t v2_path_offset = 28;  // after the int32 path length
// FILETIME counts 100 ns ticks since 1601-01-01; the Unix epoch is 11644473600 s later.
constexpr std::uint64_t epoch_ticks = 116444736000000000ULL;
constexpr std::string_view item_prefix = "recycle-bin:deleted-";
constexpr std::string_view item_suffix = "-days-ago";
constexpr std::string_view metric = "recycle_bin_aged_bytes";

// Little-endian unsigned integer of `width` bytes at `offset`.
std::uint64_t read_le(std::span<const std::uint8_t> data, std::size_t offset, std::size_t width) {
  std::uint64_t value = 0;
  for (std::size_t index = width; index > 0; --index) {
    value = (value << 8U) | data[offset + index - 1];
  }
  return value;
}

// The NUL-terminated UTF-16 path of `units` code units at `offset`; nullopt when it has no NUL
// or is empty.
std::optional<std::filesystem::path> read_path(std::span<const std::uint8_t> data,
                                               std::size_t offset, std::size_t units) {
  std::wstring path;
  for (std::size_t index = 0; index < units; ++index) {
    const auto unit = static_cast<wchar_t>(read_le(data, offset + (2 * index), 2));
    if (unit == L'\0') {
      return path.empty() ? std::nullopt : std::optional<std::filesystem::path>(path);
    }
    path.push_back(unit);
  }
  return std::nullopt;
}

}  // namespace

WallTime from_filetime(std::uint64_t ticks) {
  using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
  const auto since_epoch = static_cast<std::int64_t>(ticks - epoch_ticks);
  return WallTime{std::chrono::duration_cast<WallTime::duration>(Ticks{since_epoch})};
}

std::optional<BinRecord> parse_bin_record(std::span<const std::uint8_t> data) {
  if (data.size() < header_size) {
    return std::nullopt;
  }
  const std::uint64_t version = read_le(data, 0, 8);
  const std::uint64_t size = read_le(data, 8, 8);
  const std::uint64_t ticks = read_le(data, 16, 8);
  constexpr auto int64_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (size > int64_max || ticks > int64_max || ticks < epoch_ticks) {
    return std::nullopt;  // a negative size or time, or a deletion before 1970: not a record
  }
  std::optional<std::filesystem::path> original;
  if (version == 1 && data.size() == v1_size) {
    original = read_path(data, header_size, v1_path_units);
  } else if (version == 2 && data.size() >= v2_path_offset) {
    const std::uint64_t units = read_le(data, header_size, 4);
    if (data.size() == v2_path_offset + (2 * units)) {
      original = read_path(data, v2_path_offset, units);
    }
  }
  if (!original) {
    return std::nullopt;
  }
  return BinRecord{.size = Bytes{size}, .deleted = from_filetime(ticks), .original = *original};
}

AgedItems aged_records(std::span<const BinFile> files, WallTime now, std::chrono::days age) {
  AgedItems aged;
  for (const BinFile& file : files) {
    if (!file.has_content) {
      continue;
    }
    if (const std::optional<BinRecord> record = parse_bin_record(file.data);
        record && now - record->deleted >= age) {
      ++aged.count;
      aged.bytes += record->size;
    }
  }
  return aged;
}

std::filesystem::path recycle_bin_item(int days) {
  return from_utf8(std::string(item_prefix) + std::to_string(days) + std::string(item_suffix));
}

std::optional<int> recycle_bin_days(const std::filesystem::path& item) {
  const std::string text = safety::to_utf8(item);
  if (!text.starts_with(item_prefix) || !text.ends_with(item_suffix)) {
    return std::nullopt;
  }
  const std::string digits =
      text.substr(item_prefix.size(), text.size() - item_prefix.size() - item_suffix.size());
  if (digits.empty() || digits.size() > 5 ||
      digits.find_first_not_of("0123456789") != std::string::npos) {
    return std::nullopt;
  }
  const int days = std::stoi(digits);
  return recycle_bin_item(days) == item ? std::optional<int>(days) : std::nullopt;
}

RecycleBinCleanup::RecycleBinCleanup(IRecycleBin& bin, const os::IClock& clock, int days, Tier tier,
                                     std::stop_token stop)
    : bin_(&bin), clock_(&clock), days_(days), tier_(tier), stop_(std::move(stop)) {}

ActionInfo RecycleBinCleanup::describe() const {
  const std::string days = std::to_string(days_);
  return {.what = "Delete permanently the Recycle Bin items deleted at least " + days +
                  " days ago, on every fixed volume",
          .why = "the owner deleted them " + days + " days ago or earlier",
          .risk = Risk::low,
          .tier = tier_,
          .context = ExecutionContext::user};
}

Result<Preview> RecycleBinCleanup::preview() {
  aged_.clear();
  skipped_.clear();
  Result<std::vector<BinItem>> items = bin_->items(stop_);
  if (!items) {
    return tl::unexpected(items.error());
  }
  const WallTime now = clock_->now();
  Bytes total;
  for (BinItem& item : *items) {
    if (now - item.deleted >= std::chrono::days{days_}) {
      total += item.size;
      aged_.push_back(std::move(item));
    }
  }
  Preview preview;
  if (!aged_.empty()) {
    preview.items.push_back(PlanItem{.path = recycle_bin_item(days_), .size = total});
  }
  return preview;
}

Result<Undo> RecycleBinCleanup::apply(const Preview& preview) {
  skipped_.clear();
  before_ = Bytes{};
  Undo undo{.not_reversible = "deleted permanently"};
  for (const PlanItem& item : preview.items) {
    if (item.path != recycle_bin_item(days_)) {
      skipped_.push_back(
          {.target = safety::to_utf8(item.path),
           .error = {.code = Errc::refused, .message = "not an item of this action"}});
      continue;
    }
    if (aged_.empty()) {
      continue;
    }
    std::vector<std::string> ids;
    ids.reserve(aged_.size());
    for (const BinItem& aged : aged_) {
      ids.push_back(aged.id);
    }
    Result<std::vector<Skipped>> left = bin_->remove(ids, stop_);
    if (!left) {
      return tl::unexpected(left.error());
    }
    const std::size_t deleted = aged_.size() - std::min(left->size(), aged_.size());
    skipped_.insert(skipped_.end(), left->begin(), left->end());
    if (deleted == 0) {
      continue;
    }
    before_ += item.size;
    undo.items.push_back({.item = item.id, .data = std::to_string(deleted) + " items"});
  }
  if (undo.items.empty() && !skipped_.empty()) {
    return tl::unexpected(skipped_.front().error);
  }
  return undo;
}

Result<Verification> RecycleBinCleanup::verify(const Undo& undo) {
  if (undo.items.empty()) {
    return compare(std::string(metric), before_, Bytes{});
  }
  const Result<std::vector<BinItem>> items = bin_->items(stop_);
  if (!items) {
    return tl::unexpected(items.error());
  }
  std::set<std::string, std::less<>> deleted;
  for (const BinItem& aged : aged_) {
    deleted.insert(aged.id);
  }
  Bytes after;
  for (const BinItem& item : *items) {
    if (deleted.contains(item.id)) {
      after += item.size;
    }
  }
  return compare(std::string(metric), before_, after);
}

Result<void> RecycleBinCleanup::undo(const Undo& undo) {
  return fail(Errc::not_reversible, "not reversible: " + undo.not_reversible);
}

}  // namespace myclean::cleanup
