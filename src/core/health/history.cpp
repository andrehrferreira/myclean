#include "core/health/history.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/health/analysis.hpp"
#include "core/health/reading.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

namespace myclean::health {

namespace {

struct CounterColumn {
  std::string_view name;
  std::optional<std::int64_t> HealthReading::* member;
};

// The nullable counter columns of health_sample, in column order.
constexpr std::array<CounterColumn, 15> counter_columns{{
    {.name = "temperature_c", .member = &HealthReading::temperature_c},
    {.name = "critical_warning", .member = &HealthReading::critical_warning},
    {.name = "percentage_used", .member = &HealthReading::percentage_used},
    {.name = "available_spare", .member = &HealthReading::available_spare},
    {.name = "spare_threshold", .member = &HealthReading::spare_threshold},
    {.name = "media_errors", .member = &HealthReading::media_errors},
    {.name = "unsafe_shutdowns", .member = &HealthReading::unsafe_shutdowns},
    {.name = "bytes_written", .member = &HealthReading::bytes_written},
    {.name = "power_on_hours", .member = &HealthReading::power_on_hours},
    {.name = "reallocated_sectors", .member = &HealthReading::reallocated_sectors},
    {.name = "pending_sectors", .member = &HealthReading::pending_sectors},
    {.name = "uncorrectable_sectors", .member = &HealthReading::uncorrectable_sectors},
    {.name = "crc_errors", .member = &HealthReading::crc_errors},
    {.name = "read_errors_uncorrected", .member = &HealthReading::read_errors_uncorrected},
    {.name = "write_errors_uncorrected", .member = &HealthReading::write_errors_uncorrected},
}};

// The fixed columns before the counters, and predicts_failure after them.
constexpr std::string_view fixed_columns =
    "drive, day, time, model, firmware, disk_number, bus, source, problem, status";
constexpr int fixed_column_count = 10;

std::string column_list() {
  std::string columns(fixed_columns);
  for (const CounterColumn& column : counter_columns) {
    columns += ", ";
    columns += column.name;
  }
  return columns + ", predicts_failure";
}

const std::string& select_sql() {
  static const std::string sql = "SELECT " + column_list() + " FROM health_sample";
  return sql;
}

std::string local_day(WallTime time, const std::chrono::time_zone& zone) {
  const std::chrono::zoned_time local{&zone, std::chrono::floor<std::chrono::seconds>(time)};
  const std::chrono::year_month_day date{
      std::chrono::floor<std::chrono::days>(local.get_local_time())};
  return std::format("{:%F}", date);
}

Result<Snapshot> read_row(const store::Statement& row) {
  Snapshot snapshot{
      .day = row.column_text(1),
      .time = store::from_unix_seconds(row.column_int(2)),
      .reading = HealthReading{.drive = row.column_text(0),
                               .model = row.column_text(3),
                               .firmware = row.column_text(4),
                               .disk_number = static_cast<std::uint32_t>(row.column_int(5)),
                               .problem = row.column_text(8)}};
  const std::optional<Bus> bus = parse_bus(row.column_text(6));
  const std::optional<Source> source = parse_source(row.column_text(7));
  const std::optional<Status> status = parse_status(row.column_text(9));
  if (!bus || !source || !status) {
    return fail(Errc::invalid_data, std::format("health_sample row of '{}' on {} has an unknown "
                                                "bus, source or status",
                                                snapshot.reading.drive, snapshot.day));
  }
  snapshot.reading.bus = *bus;
  snapshot.reading.source = *source;
  snapshot.status = *status;
  int index = fixed_column_count;
  for (const CounterColumn& column : counter_columns) {
    if (!row.is_null(index)) {
      snapshot.reading.*column.member = row.column_int(index);
    }
    ++index;
  }
  if (!row.is_null(index)) {
    snapshot.reading.predicts_failure = row.column_int(index) != 0;
  }
  return snapshot;
}

// Runs `sql` with `drive` as ?1 and the given time bounds, collecting every row.
Result<std::vector<Snapshot>> query(store::Store& store, const std::string& sql,
                                    std::string_view drive, std::vector<std::int64_t> bounds) {
  std::vector<Snapshot> snapshots;
  const Result<void> read = store.read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> statement = db.prepare(sql);
    if (!statement) {
      return tl::unexpected(statement.error());
    }
    statement->bind(1, drive);
    for (std::size_t index = 0; index < bounds.size(); ++index) {
      statement->bind(static_cast<int>(index) + 2, bounds[index]);
    }
    for (;;) {
      const Result<bool> row = statement->step();
      if (!row) {
        return tl::unexpected(row.error());
      }
      if (!*row) {
        return {};
      }
      Result<Snapshot> snapshot = read_row(*statement);
      if (!snapshot) {
        return tl::unexpected(snapshot.error());
      }
      snapshots.push_back(std::move(*snapshot));
    }
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return snapshots;
}

}  // namespace

const store::MigrationSet& health_migrations() {
  static const store::MigrationSet set{.component = "health",
                                       .migrations = {
                                           {.version = 1, .name = "health_sample", .sql = R"sql(
CREATE TABLE health_sample(
  drive TEXT NOT NULL,
  day TEXT NOT NULL,
  time INTEGER NOT NULL,
  model TEXT NOT NULL,
  firmware TEXT NOT NULL,
  disk_number INTEGER NOT NULL,
  bus TEXT NOT NULL,
  source TEXT NOT NULL,
  problem TEXT NOT NULL DEFAULT '',
  status TEXT NOT NULL,
  temperature_c INTEGER,
  critical_warning INTEGER,
  percentage_used INTEGER,
  available_spare INTEGER,
  spare_threshold INTEGER,
  media_errors INTEGER,
  unsafe_shutdowns INTEGER,
  bytes_written INTEGER,
  power_on_hours INTEGER,
  reallocated_sectors INTEGER,
  pending_sectors INTEGER,
  uncorrectable_sectors INTEGER,
  crc_errors INTEGER,
  read_errors_uncorrected INTEGER,
  write_errors_uncorrected INTEGER,
  predicts_failure INTEGER,
  PRIMARY KEY(drive, day)) WITHOUT ROWID;
CREATE INDEX health_sample_time ON health_sample(time);
)sql"},
                                       }};
  return set;
}

Result<void> HealthHistory::add_retention() {
  return store_->add_retention(store::RetentionRule{
      .table = "health_sample", .time_column = "time", .max_age = store::Store::history_retention});
}

Result<void> HealthHistory::save(const HealthReading& reading, Status status, WallTime time,
                                 const std::chrono::time_zone& zone) {
  static const std::string sql = [] {
    std::string placeholders;
    for (std::size_t index = 1; index <= fixed_column_count + counter_columns.size() + 1; ++index) {
      placeholders += std::format("{}?{}", index == 1 ? "" : ", ", index);
    }
    return "INSERT OR REPLACE INTO health_sample(" + column_list() + ") VALUES(" + placeholders +
           ")";
  }();
  return store_->write([&](store::Database& db) -> Result<void> {
    Result<store::Statement> statement = db.prepare(sql);
    if (!statement) {
      return tl::unexpected(statement.error());
    }
    statement->bind(1, reading.drive)
        .bind(2, std::string_view{local_day(time, zone)})
        .bind(3, store::to_unix_seconds(time))
        .bind(4, reading.model)
        .bind(5, reading.firmware)
        .bind(6, reading.disk_number)
        .bind(7, to_string(reading.bus))
        .bind(8, to_string(reading.source))
        .bind(9, reading.problem)
        .bind(10, to_string(status));
    int index = fixed_column_count + 1;
    for (const CounterColumn& column : counter_columns) {
      if (const std::optional<std::int64_t>& value = reading.*column.member) {
        statement->bind(index, *value);
      } else {
        statement->bind(index, nullptr);
      }
      ++index;
    }
    if (reading.predicts_failure) {
      statement->bind(index, *reading.predicts_failure ? 1 : 0);
    } else {
      statement->bind(index, nullptr);
    }
    return statement->run();
  });
}

Result<std::vector<Snapshot>> HealthHistory::history(std::string_view drive, WallTime from,
                                                     WallTime to) {
  static const std::string sql =
      select_sql() + " WHERE drive = ?1 AND time >= ?2 AND time < ?3 ORDER BY time";
  return query(*store_, sql, drive, {store::to_unix_seconds(from), store::to_unix_seconds(to)});
}

Result<std::optional<WrittenAt>> HealthHistory::first_written_since(std::string_view drive,
                                                                    WallTime since) {
  static const std::string sql = select_sql() +
                                 " WHERE drive = ?1 AND time >= ?2 AND bytes_written IS NOT NULL"
                                 " ORDER BY time LIMIT 1";
  Result<std::vector<Snapshot>> rows = query(*store_, sql, drive, {store::to_unix_seconds(since)});
  if (!rows) {
    return tl::unexpected(rows.error());
  }
  if (rows->empty()) {
    return std::optional<WrittenAt>{};
  }
  const Snapshot& first = rows->front();
  return std::optional<WrittenAt>{
      WrittenAt{.time = first.time, .bytes = *first.reading.bytes_written}};
}

}  // namespace myclean::health
