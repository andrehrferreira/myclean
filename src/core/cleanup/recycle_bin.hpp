#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/cleanup/handler_cleanup.hpp"
#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"

// The Recycle Bin items the owner deleted long ago (cleaning spec, "Aged Recycle Bin Items"):
// a "Recycle Bin" handler case with require.min_age_days removes, on every fixed volume, the
// items deleted at least that many days ago, judged by their deletion time, never by file times.
// The service sizes the plan from the owner's $I records (IBinRecords); the tray deletes the
// items through the shell's Recycle Bin folder (IRecycleBin). The deny-list keeps
// ?:\$Recycle.Bin out of SafeRemove, so neither goes through the rule engine's delete path.
namespace myclean::cleanup {

// The VolumeCaches key name a clean case uses for the Recycle Bin.
inline constexpr std::string_view recycle_bin_handler = "Recycle Bin";
// The shortest deletion age acted on: the auto tier's floor. The service proposes, and the tray
// runs, nothing for a younger one.
inline constexpr int min_bin_days = 7;

// ---- The owner's $I records (the service) -----------------------------------------------------

// What Explorer records in the $I file of each deleted item, next to its $R content:
//   int64 version (1 or 2), int64 size, FILETIME deletion time (offset 16), then the original
//   path: version 1 260 UTF-16 code units (NUL-padded, 544 bytes in all); version 2 an int32
//   length in code units (with the final NUL) and the path (28 + 2 * length bytes in all).
struct BinRecord {
  Bytes size;  // a folder: everything in it
  WallTime deleted;
  std::filesystem::path original;
};

// The largest $I record: version 2 with a path of 32767 code units and its NUL.
inline constexpr std::size_t max_bin_record = 28 + (2 * 32768);

// FILETIME ticks (100 ns since 1601-01-01 UTC) as a point in time.
[[nodiscard]] WallTime from_filetime(std::uint64_t ticks);

// The record in `data`; nullopt for an unknown version, a truncated or oversized record, or a
// path that does not fit or is not NUL-terminated.
[[nodiscard]] std::optional<BinRecord> parse_bin_record(std::span<const std::uint8_t> data);

// One $I file of the owner's bin on a fixed volume.
struct BinFile {
  std::filesystem::path path;      // ?:\$Recycle.Bin\<owner SID>\$I<name>
  std::vector<std::uint8_t> data;  // its first max_bin_record bytes
  bool has_content = false;        // its $R<name> still exists (Windows may have removed it)
};

// Reads the $I files of the owner's bin on every fixed volume. Implemented by platform/cleanup
// (Win32BinRecords) and by test fakes.
class IBinRecords {
 public:
  virtual ~IBinRecords() = default;
  IBinRecords(const IBinRecords&) = delete;
  IBinRecords& operator=(const IBinRecords&) = delete;
  IBinRecords(IBinRecords&&) = delete;
  IBinRecords& operator=(IBinRecords&&) = delete;

  // A volume without a bin of the owner adds nothing; fails when a bin cannot be read, or with
  // refused once `stop` is requested.
  [[nodiscard]] virtual Result<std::vector<BinFile>> read(const std::stop_token& stop) const = 0;

 protected:
  IBinRecords() = default;
};

struct AgedItems {
  std::int64_t count = 0;
  Bytes bytes;  // their recorded sizes
};

// The items of `files` deleted at least `age` before `now`. Records that do not parse, and
// records whose content is gone (they use no space), are not counted.
[[nodiscard]] AgedItems aged_records(std::span<const BinFile> files, WallTime now,
                                     std::chrono::days age);

// The plan item that stands for the bin items deleted at least `days` days ago:
// "recycle-bin:deleted-<days>-days-ago". Not a file system path, so SafeRemove refuses it.
[[nodiscard]] std::filesystem::path recycle_bin_item(int days);
// The days of a recycle_bin_item() path; nullopt for any other path.
[[nodiscard]] std::optional<int> recycle_bin_days(const std::filesystem::path& item);

// ---- The shell's Recycle Bin folder (the tray) ------------------------------------------------

// One item of the Recycle Bin folder.
struct BinItem {
  std::string id;    // its parsing name (the $R path); unique while it is in the bin
  std::string name;  // where it was deleted from and its name, for the owner
  WallTime deleted;  // System.Recycle.DateDeleted
  Bytes size;
};

// The owner's Recycle Bin through the shell (FOLDERID_RecycleBinFolder), with no UI at all.
// Implemented by platform/cleanup (Win32RecycleBin) and by test fakes.
class IRecycleBin {
 public:
  virtual ~IRecycleBin() = default;
  IRecycleBin(const IRecycleBin&) = delete;
  IRecycleBin& operator=(const IRecycleBin&) = delete;
  IRecycleBin(IRecycleBin&&) = delete;
  IRecycleBin& operator=(IRecycleBin&&) = delete;

  // The items of the bins of the fixed volumes.
  [[nodiscard]] virtual Result<std::vector<BinItem>> items(const std::stop_token& stop) = 0;

  // Deletes the items with `ids` permanently. What was left out, by item name: an item no longer
  // in the bin, one the shell could not delete, every one not reached once `stop` is requested.
  // Fails when the bin cannot be read.
  [[nodiscard]] virtual Result<std::vector<Skipped>> remove(std::span<const std::string> ids,
                                                            const std::stop_token& stop) = 0;

 protected:
  IRecycleBin() = default;
};

// Deletes the Recycle Bin items the owner deleted at least `days` days ago, permanently, in the
// owner's context (the tray). Not reversible. Every reference must outlive the action.
class RecycleBinCleanup final : public Action {
 public:
  RecycleBinCleanup(IRecycleBin& bin, const os::IClock& clock, int days, Tier tier,
                    std::stop_token stop = {});

  [[nodiscard]] ActionInfo describe() const override;
  // One recycle_bin_item(days) item, sized by the aged items it stands for; none when there are
  // none. Fails when the bin cannot be read.
  [[nodiscard]] Result<Preview> preview() override;
  // Deletes the aged items of the last preview when `preview` holds its item. Undo: that item
  // (data: how many items were deleted); not_reversible "deleted permanently". Fails when items
  // were to be deleted and none was.
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  // Metric "recycle_bin_aged_bytes": the previewed bytes against those of the deleted items
  // still in the bin.
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  // Always fails with not_reversible.
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

  [[nodiscard]] const std::vector<Skipped>& skipped() const { return skipped_; }

 private:
  IRecycleBin* bin_;
  const os::IClock* clock_;
  int days_;
  Tier tier_;
  std::stop_token stop_;
  std::vector<BinItem> aged_;  // of the last preview
  std::vector<Skipped> skipped_;
  Bytes before_;
};

}  // namespace myclean::cleanup
