#include "core/resident/history.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/resident/sampler.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

namespace myclean::resident {
namespace {

WallTime floor_to(WallTime time, std::chrono::minutes step) {
  const auto minutes = std::chrono::floor<std::chrono::minutes>(time.time_since_epoch());
  return WallTime{minutes - minutes % step};
}

std::int64_t stored(std::uint64_t value) {
  return static_cast<std::int64_t>(value);
}

constexpr std::string_view upsert_bucket = R"sql(
INSERT INTO resident_fine(key, time, private_bytes, samples) VALUES (?1, ?2, ?3, ?4)
ON CONFLICT(key, time) DO UPDATE SET
  private_bytes = (private_bytes * samples + excluded.private_bytes * excluded.samples) /
                  (samples + excluded.samples),
  samples = samples + excluded.samples)sql";

constexpr std::string_view upsert_hour = R"sql(
INSERT INTO resident_hour(key, time, minutes, cpu_seconds, ram_sum, ram_peak, disk_bytes,
                          net_bytes)
VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
ON CONFLICT(key, time) DO UPDATE SET
  minutes = minutes + excluded.minutes,
  cpu_seconds = cpu_seconds + excluded.cpu_seconds,
  ram_sum = ram_sum + excluded.ram_sum,
  ram_peak = MAX(ram_peak, excluded.ram_peak),
  disk_bytes = disk_bytes + excluded.disk_bytes,
  net_bytes = net_bytes + excluded.net_bytes)sql";

}  // namespace

const store::MigrationSet& resident_migrations() {
  static const store::MigrationSet set{.component = "resident",
                                       .migrations = {
                                           {.version = 1, .name = "resident_history", .sql = R"sql(
CREATE TABLE resident_fine(
  key TEXT NOT NULL,
  time INTEGER NOT NULL,
  private_bytes INTEGER NOT NULL,
  samples INTEGER NOT NULL,
  PRIMARY KEY (key, time)
);
CREATE INDEX resident_fine_time ON resident_fine(time);
CREATE TABLE resident_hour(
  key TEXT NOT NULL,
  time INTEGER NOT NULL,
  minutes INTEGER NOT NULL,
  cpu_seconds REAL NOT NULL,
  ram_sum INTEGER NOT NULL,
  ram_peak INTEGER NOT NULL,
  disk_bytes INTEGER NOT NULL,
  net_bytes INTEGER NOT NULL,
  PRIMARY KEY (key, time)
);
CREATE INDEX resident_hour_time ON resident_hour(time);
)sql"},
                                       }};
  return set;
}

std::vector<store::RetentionRule> resident_retention() {
  return {{.table = "resident_fine", .time_column = "time", .max_age = fine_span},
          {.table = "resident_hour", .time_column = "time", .max_age = hour_span}};
}

Result<void> History::load(WallTime now) {
  std::map<std::string, std::vector<Point>, std::less<>> trends;
  Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> statement = db.prepare(
        "SELECT key, time, private_bytes, samples FROM resident_fine WHERE time >= ?1 "
        "ORDER BY key, time");
    if (!statement) {
      return tl::unexpected(statement.error());
    }
    statement->bind(1, store::to_unix_seconds(now - fine_span));
    for (;;) {
      Result<bool> row = statement->step();
      if (!row) {
        return tl::unexpected(row.error());
      }
      if (!*row) {
        return {};
      }
      trends[statement->column_text(0)].push_back(
          Point{.time = store::from_unix_seconds(statement->column_int(1)),
                .private_bytes = Bytes{static_cast<std::uint64_t>(statement->column_int(2))},
                .samples = static_cast<int>(statement->column_int(3))});
    }
  });
  if (!read) {
    return read;
  }
  trends_ = std::move(trends);
  return {};
}

bool History::add(WallTime time, std::span<const MinuteUse> use) {
  const WallTime bucket_at = floor_to(time, bucket);
  const WallTime hour_at = floor_to(time, std::chrono::hours{1});
  const bool closed = bucket_start_ && *bucket_start_ != bucket_at;
  if (closed) {
    close_bucket();
    const WallTime oldest = bucket_at - fine_span;
    std::erase_if(trends_, [&](auto& entry) {
      std::erase_if(entry.second, [&](const Point& point) { return point.time < oldest; });
      return entry.second.empty();
    });
  }
  if (hour_start_ && *hour_start_ != hour_at) {
    close_hour();
  }
  bucket_start_ = bucket_at;
  hour_start_ = hour_at;
  for (const MinuteUse& entry : use) {
    HourSum& hour = hour_[entry.key];
    if (entry.running) {
      BucketSum& sum = bucket_[entry.key];
      sum.private_sum += entry.private_bytes.count();
      sum.samples += 1;
      hour.minutes += 1;
      hour.ram_sum += entry.ram.count();
      hour.ram_peak = std::max(hour.ram_peak, entry.ram.count());
    }
    hour.cpu_seconds += entry.cpu_seconds;
    hour.disk_bytes += entry.disk_bytes;
    hour.net_bytes += entry.net_bytes;
  }
  return closed;
}

void History::flush() {
  close_bucket();
  close_hour();
}

void History::close_bucket() {
  if (!bucket_start_ || bucket_.empty()) {
    return;
  }
  const WallTime start = *bucket_start_;
  for (const auto& [key, sum] : bucket_) {
    const Point point{
        .time = start,
        .private_bytes = Bytes{sum.private_sum / static_cast<std::uint64_t>(sum.samples)},
        .samples = sum.samples};
    store_->enqueue(std::string(upsert_bucket),
                    {key, store::to_unix_seconds(start), stored(point.private_bytes.count()),
                     std::int64_t{point.samples}});
    std::vector<Point>& trend = trends_[key];
    if (!trend.empty() && trend.back().time == start) {
      Point& merged = trend.back();
      const auto samples =
          static_cast<std::uint64_t>(merged.samples) + static_cast<std::uint64_t>(point.samples);
      merged.private_bytes =
          Bytes{(merged.private_bytes.count() * static_cast<std::uint64_t>(merged.samples) +
                 sum.private_sum) /
                samples};
      merged.samples += point.samples;
    } else {
      trend.push_back(point);
    }
  }
  bucket_.clear();
}

void History::close_hour() {
  if (!hour_start_) {
    return;
  }
  for (const auto& [key, sum] : hour_) {
    store_->enqueue(
        std::string(upsert_hour),
        {key, store::to_unix_seconds(*hour_start_), sum.minutes, sum.cpu_seconds,
         stored(sum.ram_sum), stored(sum.ram_peak), stored(sum.disk_bytes), stored(sum.net_bytes)});
  }
  hour_.clear();
}

Result<std::vector<Cost>> costs(store::Store& store, WallTime from,
                                std::chrono::minutes min_running) {
  std::vector<Cost> list;
  Result<void> read = store.read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> statement = db.prepare(R"sql(
SELECT key, SUM(minutes), SUM(cpu_seconds), SUM(ram_sum), MAX(ram_peak), SUM(disk_bytes),
       SUM(net_bytes)
FROM resident_hour WHERE time >= ?1
GROUP BY key HAVING SUM(minutes) >= ?2 AND SUM(minutes) > 0
ORDER BY SUM(cpu_seconds) DESC, key)sql");
    if (!statement) {
      return tl::unexpected(statement.error());
    }
    statement->bind(1, store::to_unix_seconds(from)).bind(2, min_running.count());
    for (;;) {
      Result<bool> row = statement->step();
      if (!row) {
        return tl::unexpected(row.error());
      }
      if (!*row) {
        return {};
      }
      const std::int64_t minutes = statement->column_int(1);
      list.push_back(
          Cost{.key = statement->column_text(0),
               .running = std::chrono::minutes{minutes},
               .cpu_seconds = statement->column_double(2),
               .ram_average = Bytes{static_cast<std::uint64_t>(statement->column_int(3) / minutes)},
               .ram_peak = Bytes{static_cast<std::uint64_t>(statement->column_int(4))},
               .disk_bytes = static_cast<std::uint64_t>(statement->column_int(5)),
               .net_bytes = static_cast<std::uint64_t>(statement->column_int(6))});
    }
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return list;
}

}  // namespace myclean::resident
