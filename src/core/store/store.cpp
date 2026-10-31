#include "core/store/store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/schema.hpp"
#include "core/store/time_series.hpp"

namespace myclean::store {
namespace {

// Tables whose rows are permanent (store specification, Retention).
constexpr std::array<std::string_view, 5> permanent_tables = {"action_log", "undo_blob", "schedule",
                                                              "snapshot", "report"};

bool is_identifier(std::string_view name) {
  return !name.empty() && (name.front() < '0' || name.front() > '9') &&
         std::ranges::all_of(name, [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
         });
}

void keep_first_error(Result<void>& result, Result<void> step) {
  if (result && !step) {
    result = std::move(step);
  }
}

}  // namespace

std::filesystem::path database_path(const std::filesystem::path& program_data) {
  return program_data / "MyClean" / "myclean.db";
}

Result<std::unique_ptr<Store>> Store::open(const std::filesystem::path& path,
                                           const os::IClock& clock,
                                           std::span<const MigrationSet> migrations) {
  Result<Database> db = Database::open(path);
  if (!db) {
    return tl::unexpected(db.error());
  }
  if (Result<void> migrated = migrate(*db, store_migrations()); !migrated) {
    return tl::unexpected(migrated.error());
  }
  for (const MigrationSet& set : migrations) {
    if (Result<void> migrated = migrate(*db, set); !migrated) {
      return tl::unexpected(migrated.error());
    }
  }
  return std::unique_ptr<Store>(new Store(std::move(*db), clock));
}

Store::Store(Database database, const os::IClock& clock)
    : clock_(clock),
      db_(std::move(database)),
      retention_{{.table = "ts_minute", .time_column = "time", .max_age = minute_retention},
                 {.table = "ts_hour", .time_column = "time", .max_age = history_retention},
                 {.table = "plan", .time_column = "created_at", .max_age = history_retention},
                 {.table = "finding", .time_column = "detected_at", .max_age = history_retention}},
      last_flush_(clock.monotonic()) {}

Store::~Store() {
  static_cast<void>(close());
}

Result<void> Store::write(const std::function<Result<void>(Database&)>& work) {
  const std::lock_guard lock(db_mutex_);
  return db_.transaction([&] { return work(db_); });
}

Result<void> Store::read(const std::function<Result<void>(Database&)>& work) {
  const std::lock_guard lock(db_mutex_);
  return work(db_);
}

void Store::enqueue(std::string sql, std::vector<Value> params) {
  const std::lock_guard lock(buffer_mutex_);
  pending_.push_back(BufferedWrite{.sql = std::move(sql), .params = std::move(params)});
}

Result<void> Store::flush() {
  std::vector<BufferedWrite> batch;
  {
    const std::lock_guard lock(buffer_mutex_);
    batch.swap(pending_);
  }
  const std::lock_guard lock(db_mutex_);
  last_flush_ = clock_.monotonic();
  if (batch.empty()) {
    return {};
  }
  const Result<void> written = db_.transaction([&]() -> Result<void> {
    std::map<std::string, Statement, std::less<>> statements;
    for (const BufferedWrite& write : batch) {
      auto statement = statements.find(write.sql);
      if (statement == statements.end()) {
        Result<Statement> prepared = db_.prepare(write.sql);
        if (!prepared) {
          return tl::unexpected(prepared.error());
        }
        statement = statements.emplace(write.sql, std::move(*prepared)).first;
      }
      for (std::size_t index = 0; index < write.params.size(); ++index) {
        statement->second.bind_value(static_cast<int>(index) + 1, write.params[index]);
      }
      if (Result<void> ran = statement->second.run(); !ran) {
        return ran;
      }
    }
    return {};
  });
  if (!written) {
    return fail(written.error().code, std::to_string(batch.size()) +
                                          " buffered writes were lost: " + written.error().message);
  }
  return {};
}

void Store::record(const SeriesKey& key, Sample sample) {
  const std::lock_guard lock(buffer_mutex_);
  series_.add(key, sample);
}

std::vector<Sample> Store::raw(const SeriesKey& key) const {
  const std::lock_guard lock(buffer_mutex_);
  return series_.raw(key, clock_.now());
}

Result<std::vector<Aggregate>> Store::minutes(const SeriesKey& key, WallTime from, WallTime to) {
  return aggregates("ts_minute", key, from, to);
}

Result<std::vector<Aggregate>> Store::hours(const SeriesKey& key, WallTime from, WallTime to) {
  return aggregates("ts_hour", key, from, to);
}

Result<std::vector<Aggregate>> Store::aggregates(std::string_view table, const SeriesKey& key,
                                                 WallTime from, WallTime to) {
  const std::lock_guard lock(db_mutex_);
  Result<Statement> query = db_.prepare(
      "SELECT a.time, a.min, a.max, a.mean, a.count FROM " + std::string(table) +
      " a JOIN series s ON s.id = a.series "
      "WHERE s.metric = ?1 AND s.entity = ?2 AND a.time >= ?3 AND a.time < ?4 ORDER BY a.time");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, key.metric)
      .bind(2, key.entity)
      .bind(3, to_unix_seconds(from))
      .bind(4, to_unix_seconds(to));
  std::vector<Aggregate> rows;
  Result<bool> row = query->step();
  for (; row && *row; row = query->step()) {
    rows.push_back(Aggregate{.time = from_unix_seconds(query->column_int(0)),
                             .min = query->column_double(1),
                             .max = query->column_double(2),
                             .mean = query->column_double(3),
                             .count = query->column_int(4)});
  }
  if (!row) {
    return tl::unexpected(row.error());
  }
  return rows;
}

Result<void> Store::aggregate_hours(WallTime until) {
  const std::lock_guard lock(db_mutex_);
  if (hours_until_ && until <= *hours_until_) {
    return {};
  }
  std::int64_t from = 0;
  if (hours_until_) {
    from = to_unix_seconds(*hours_until_);
  } else {
    Result<Statement> latest = db_.prepare("SELECT MAX(time) FROM ts_hour");
    if (!latest) {
      return tl::unexpected(latest.error());
    }
    if (Result<bool> row = latest->step(); !row) {
      return tl::unexpected(row.error());
    }
    if (!latest->is_null(0)) {
      from = latest->column_int(0) + std::chrono::seconds{std::chrono::hours{1}}.count();
    }
  }
  // A minute's mean weighs by its sample count, so the hour mean is the mean of all samples.
  Result<Statement> insert = db_.prepare(
      "INSERT OR REPLACE INTO ts_hour(series, time, min, max, mean, count) "
      "SELECT series, time - time % 3600 AS hour, MIN(min), MAX(max), "
      "SUM(mean * count) / SUM(count), SUM(count) FROM ts_minute "
      "WHERE time >= ?1 AND time < ?2 GROUP BY series, hour");
  if (!insert) {
    return tl::unexpected(insert.error());
  }
  if (Result<void> ran = insert->bind(1, from).bind(2, to_unix_seconds(until)).run(); !ran) {
    return ran;
  }
  hours_until_ = until;
  return {};
}

Result<void> Store::add_retention(RetentionRule rule) {
  if (!is_identifier(rule.table) || !is_identifier(rule.time_column)) {
    return fail(Errc::invalid_argument, "retention rule names an invalid table or column: " +
                                            rule.table + "." + rule.time_column);
  }
  if (std::ranges::find(permanent_tables, rule.table) != permanent_tables.end()) {
    return fail(Errc::refused, "rows of " + rule.table + " are permanent and never retained");
  }
  const std::lock_guard lock(db_mutex_);
  retention_.push_back(std::move(rule));
  return {};
}

Result<void> Store::run_retention() {
  const WallTime now = clock_.now();
  const std::lock_guard lock(db_mutex_);
  last_retention_ = clock_.monotonic();
  return db_.transaction([&]() -> Result<void> {
    for (const RetentionRule& rule : retention_) {
      Result<Statement> remove =
          db_.prepare("DELETE FROM " + rule.table + " WHERE " + rule.time_column + " < ?1");
      if (!remove) {
        return tl::unexpected(remove.error());
      }
      if (Result<void> ran = remove->bind(1, to_unix_seconds(now - rule.max_age)).run(); !ran) {
        return ran;
      }
    }
    return {};
  });
}

Result<void> Store::tick() {
  const WallTime settled = clock_.now() - close_delay;
  const MonoTime now = clock_.monotonic();
  {
    const std::lock_guard lock(buffer_mutex_);
    for (const ClosedMinute& minute : series_.take_closed(settled)) {
      const SeriesKey& key = minute.key;
      const Aggregate& value = minute.aggregate;
      pending_.push_back(
          BufferedWrite{.sql = "INSERT OR IGNORE INTO series(metric, entity) VALUES(?1, ?2)",
                        .params = {key.metric, key.entity}});
      pending_.push_back(BufferedWrite{
          .sql = "INSERT OR REPLACE INTO ts_minute(series, time, min, max, mean, count) "
                 "VALUES((SELECT id FROM series WHERE metric = ?1 AND entity = ?2), "
                 "?3, ?4, ?5, ?6, ?7)",
          .params = {key.metric, key.entity, to_unix_seconds(value.time), value.min, value.max,
                     value.mean, value.count}});
    }
    series_.evict(clock_.now());
  }

  const WallTime closed_hours = std::chrono::floor<std::chrono::hours>(settled);
  bool hour_due = false;
  bool flush_due = false;
  bool retention_due = false;
  {
    const std::lock_guard lock(db_mutex_);
    hour_due = !hours_until_ || closed_hours > *hours_until_;
    flush_due = now - last_flush_ >= flush_interval;
    retention_due = !last_retention_ || now - *last_retention_ >= retention_interval;
  }

  Result<void> result;
  if (hour_due || flush_due) {
    // An hour is aggregated from ts_minute, so its minutes are committed first.
    keep_first_error(result, flush());
  }
  if (hour_due) {
    keep_first_error(result, aggregate_hours(closed_hours));
  }
  if (retention_due) {
    keep_first_error(result, run_retention());
  }
  return result;
}

Result<void> Store::close() {
  return flush();
}

}  // namespace myclean::store
