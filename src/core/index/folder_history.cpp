#include "core/index/folder_history.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/index/volume_index.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/text.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

namespace myclean::index {

namespace {

std::filesystem::path path_from_utf8(const std::string& text) {
  return std::filesystem::path{std::u8string(text.begin(), text.end())};
}

}  // namespace

const store::MigrationSet& index_migrations() {
  static const store::MigrationSet set{.component = "index",
                                       .migrations = {
                                           {.version = 1, .name = "index_folder_size", .sql = R"sql(
CREATE TABLE index_folder_size(
  volume TEXT NOT NULL,
  time INTEGER NOT NULL,
  path TEXT NOT NULL,
  size_on_disk INTEGER NOT NULL,
  size INTEGER NOT NULL,
  PRIMARY KEY (volume, time, path)
);
)sql"},
                                       }};
  return set;
}

std::vector<FolderGrowth> growth(std::span<const FolderSize> before,
                                 std::span<const FolderSize> now, std::size_t limit) {
  std::unordered_map<std::string, std::uint64_t> previous;
  for (const FolderSize& folder : before) {
    previous.emplace(space::utf8(folder.path), folder.size_on_disk.count());
  }
  struct Grown {
    const FolderSize* folder;
    std::string key;
    std::uint64_t before;
    std::uint64_t delta;
  };
  std::vector<Grown> grown;
  for (const FolderSize& folder : now) {
    std::string key = space::utf8(folder.path);
    const auto found = previous.find(key);
    const std::uint64_t old = found == previous.end() ? 0 : found->second;
    if (folder.size_on_disk.count() > old) {
      grown.push_back(Grown{.folder = &folder,
                            .key = std::move(key),
                            .before = old,
                            .delta = folder.size_on_disk.count() - old});
    }
  }
  // The largest growth of a direct subfolder, by folder.
  std::unordered_map<std::string, std::uint64_t> largest_child;
  for (const Grown& entry : grown) {
    std::string parent = space::utf8(entry.folder->path.parent_path());
    if (parent != entry.key) {
      std::uint64_t& largest = largest_child[std::move(parent)];
      largest = std::max(largest, entry.delta);
    }
  }
  std::erase_if(grown, [&largest_child](const Grown& entry) {
    const auto child = largest_child.find(entry.key);
    return child != largest_child.end() && child->second * 10 >= entry.delta * 9;
  });
  std::ranges::sort(grown, [](const Grown& left, const Grown& right) {
    return left.delta != right.delta ? left.delta > right.delta : left.key < right.key;
  });
  std::vector<FolderGrowth> result;
  for (const Grown& entry : grown) {
    if (result.size() == limit) {
      break;
    }
    result.push_back(FolderGrowth{.path = entry.folder->path,
                                  .before = Bytes{entry.before},
                                  .after = entry.folder->size_on_disk});
  }
  return result;
}

Result<void> FolderHistory::add_retention() {
  return store_->add_retention(store::RetentionRule{
      .table = "index_folder_size",
      .time_column = "time",
      .max_age = std::chrono::duration_cast<std::chrono::seconds>(snapshot_retention)});
}

Result<void> FolderHistory::save(std::string_view volume, WallTime time,
                                 std::span<const FolderSize> folders) {
  const std::int64_t seconds = store::to_unix_seconds(time);
  return store_->write([&](store::Database& db) -> Result<void> {
    Result<store::Statement> clear =
        db.prepare("DELETE FROM index_folder_size WHERE volume = ?1 AND time = ?2");
    if (!clear) {
      return tl::unexpected(clear.error());
    }
    if (Result<void> cleared = clear->bind(1, volume).bind(2, seconds).run(); !cleared) {
      return cleared;
    }
    Result<store::Statement> insert = db.prepare(
        "INSERT INTO index_folder_size(volume, time, path, size_on_disk, size) "
        "VALUES (?1, ?2, ?3, ?4, ?5)");
    if (!insert) {
      return tl::unexpected(insert.error());
    }
    for (const FolderSize& folder : folders) {
      Result<void> inserted = insert->bind(1, volume)
                                  .bind(2, seconds)
                                  .bind(3, std::string_view{space::utf8(folder.path)})
                                  .bind(4, static_cast<std::int64_t>(folder.size_on_disk.count()))
                                  .bind(5, static_cast<std::int64_t>(folder.size.count()))
                                  .run();
      if (!inserted) {
        return inserted;
      }
    }
    return {};
  });
}

Result<std::optional<FolderSnapshot>> FolderHistory::snapshot_at(std::string_view volume,
                                                                 WallTime time) {
  std::optional<FolderSnapshot> snapshot;
  const Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> latest =
        db.prepare("SELECT MAX(time) FROM index_folder_size WHERE volume = ?1 AND time <= ?2");
    if (!latest) {
      return tl::unexpected(latest.error());
    }
    latest->bind(1, volume).bind(2, store::to_unix_seconds(time));
    const Result<bool> row = latest->step();
    if (!row) {
      return tl::unexpected(row.error());
    }
    if (!*row || latest->is_null(0)) {
      return {};
    }
    const std::int64_t taken = latest->column_int(0);
    Result<store::Statement> rows = db.prepare(
        "SELECT path, size_on_disk, size FROM index_folder_size "
        "WHERE volume = ?1 AND time = ?2 ORDER BY path");
    if (!rows) {
      return tl::unexpected(rows.error());
    }
    rows->bind(1, volume).bind(2, taken);
    snapshot = FolderSnapshot{.time = store::from_unix_seconds(taken)};
    for (;;) {
      const Result<bool> next = rows->step();
      if (!next) {
        return tl::unexpected(next.error());
      }
      if (!*next) {
        return {};
      }
      snapshot->folders.push_back(
          FolderSize{.path = path_from_utf8(rows->column_text(0)),
                     .size_on_disk = Bytes{static_cast<std::uint64_t>(rows->column_int(1))},
                     .size = Bytes{static_cast<std::uint64_t>(rows->column_int(2))}});
    }
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return snapshot;
}

Result<std::optional<GrowthReport>> FolderHistory::growth_over(std::string_view volume,
                                                               WallTime now,
                                                               std::chrono::seconds period,
                                                               std::span<const FolderSize> current,
                                                               std::size_t limit) {
  const Result<std::optional<FolderSnapshot>> baseline = snapshot_at(volume, now - period);
  if (!baseline) {
    return tl::unexpected(baseline.error());
  }
  const std::optional<FolderSnapshot>& snapshot = *baseline;
  if (!snapshot) {
    return std::optional<GrowthReport>{};
  }
  return std::optional<GrowthReport>{
      GrowthReport{.since = snapshot->time, .folders = growth(snapshot->folders, current, limit)}};
}

}  // namespace myclean::index
