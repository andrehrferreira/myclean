#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/index/volume_index.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

namespace myclean::index {

// The index tables (component "index"), registered with store::Store::open:
//   index_folder_size  one row per volume, snapshot time and folder of at least
//                      snapshot_min_size: its size on disk and logical size. Kept 32 days, so a
//                      30-day baseline is always there.
[[nodiscard]] const store::MigrationSet& index_migrations();

// Folders smaller than this are not stored; growth from below it counts from zero, so a
// reported growth is at most this much too large.
inline constexpr Bytes snapshot_min_size{std::uint64_t{32} << 20U};
inline constexpr std::chrono::days snapshot_retention{32};

struct FolderSnapshot {
  WallTime time;
  std::vector<FolderSize> folders;
};

struct FolderGrowth {
  std::filesystem::path path;
  Bytes before;
  Bytes after;

  friend bool operator==(const FolderGrowth&, const FolderGrowth&) = default;
};

// The folders that grew from `before` to `now`, most growth first, at most `limit`. A folder
// missing from `before` counts as empty there. A folder is left out when one of its subfolders
// accounts for at least 90 % of its growth: that subfolder is the one listed.
[[nodiscard]] std::vector<FolderGrowth> growth(std::span<const FolderSize> before,
                                               std::span<const FolderSize> now, std::size_t limit);

struct GrowthReport {
  WallTime since;  // the time of the snapshot compared with
  std::vector<FolderGrowth> folders;
};

// Per-folder sizes of each volume over time. Every call is one synchronous Store::write or
// Store::read.
class FolderHistory {
 public:
  // `store` must outlive the history.
  explicit FolderHistory(store::Store& store) : store_(&store) {}

  // Adds the 32-day retention rule for index_folder_size to the store.
  [[nodiscard]] Result<void> add_retention();

  // Stores `folders` (typically VolumeIndex::folder_sizes(snapshot_min_size)) as the snapshot
  // of `volume` ("C:") taken at `time`, replacing one taken at the same second.
  [[nodiscard]] Result<void> save(std::string_view volume, WallTime time,
                                  std::span<const FolderSize> folders);
  // The newest snapshot of `volume` taken at or before `time`; nullopt when there is none.
  [[nodiscard]] Result<std::optional<FolderSnapshot>> snapshot_at(std::string_view volume,
                                                                  WallTime time);
  // What grew on `volume` over `period` (24 h, 7 days, 30 days) up to `now`, whose folder
  // sizes are `current`: compared with the newest snapshot at least `period` old; nullopt when
  // the history is not that long yet.
  [[nodiscard]] Result<std::optional<GrowthReport>> growth_over(std::string_view volume,
                                                                WallTime now,
                                                                std::chrono::seconds period,
                                                                std::span<const FolderSize> current,
                                                                std::size_t limit);

 private:
  store::Store* store_;
};

}  // namespace myclean::index
