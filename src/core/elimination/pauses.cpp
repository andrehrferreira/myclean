#include "core/elimination/pauses.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/elimination/suspects.hpp"
#include "core/model/enum_names.hpp"
#include "core/model/ids.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/process_control.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"

namespace myclean::elimination {
namespace {

constexpr std::array<std::string_view, 6> reason_names = {
    "period_ended", "ended_by_owner", "undone", "service_restarted", "service_stopped", "failed"};

double hours_of(WallTime from, WallTime to) {
  return std::chrono::duration<double, std::chrono::hours::period>(to - from).count();
}

std::int64_t nanoseconds_of(WallTime time) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

WallTime from_nanoseconds(std::int64_t nanoseconds) {
  return WallTime{
      std::chrono::duration_cast<WallTime::duration>(std::chrono::nanoseconds{nanoseconds})};
}

constexpr std::string_view pause_columns =
    "id, plan_id, image, mode, started, ends, restored, end_reason, baseline_incidents, "
    "baseline_hours, pause_incidents, pause_hours";

Result<PauseRecord> read_pause(const store::Statement& row) {
  const std::optional<PauseMode> mode = parse_pause_mode(row.column_text(3));
  if (!mode) {
    return fail(Errc::invalid_data, "suspect_pause " + std::to_string(row.column_int(0)) +
                                        " has an unknown mode '" + row.column_text(3) + "'");
  }
  PauseRecord record{
      .id = row.column_int(0),
      .plan = PlanId{row.column_int(1)},
      .image = row.column_text(2),
      .mode = *mode,
      .started = store::from_unix_seconds(row.column_int(4)),
      .ends = store::from_unix_seconds(row.column_int(5)),
      .baseline = Rate{.incidents = row.column_int(8), .hours = row.column_double(9)}};
  if (!row.is_null(6)) {
    record.restored = store::from_unix_seconds(row.column_int(6));
    record.reason = parse_end_reason(row.column_text(7));
    record.during = Rate{.incidents = row.is_null(10) ? 0 : row.column_int(10),
                         .hours = row.is_null(11) ? 0.0 : row.column_double(11)};
  }
  return record;
}

Result<std::vector<PauseRecord>> query_pauses(store::Database& db, std::string_view where,
                                              const std::optional<std::int64_t>& value,
                                              std::size_t limit) {
  Result<store::Statement> query =
      db.prepare("SELECT " + std::string(pause_columns) + " FROM suspect_pause " +
                 std::string(where) + " ORDER BY id DESC LIMIT ?2");
  if (!query) {
    return tl::unexpected(query.error());
  }
  if (value) {
    query->bind(1, *value);
  }
  query->bind(2, static_cast<std::int64_t>(limit));
  std::vector<PauseRecord> records;
  Result<bool> row = query->step();
  for (; row && *row; row = query->step()) {
    Result<PauseRecord> record = read_pause(*query);
    if (!record) {
      return tl::unexpected(record.error());
    }
    records.push_back(*std::move(record));
  }
  if (!row) {
    return tl::unexpected(row.error());
  }
  return records;
}

Result<std::optional<PauseRecord>> first_of(Result<std::vector<PauseRecord>> records) {
  if (!records) {
    return tl::unexpected(records.error());
  }
  if (records->empty()) {
    return std::optional<PauseRecord>();
  }
  return std::optional<PauseRecord>(std::move(records->front()));
}

}  // namespace

std::optional<double> Rate::per_day() const {
  if (hours <= 0.0) {
    return std::nullopt;
  }
  return static_cast<double>(incidents) * 24.0 / hours;
}

Rate rate_in(std::span<const WallTime> times, std::span<const Span> spans) {
  Rate rate;
  for (const Span& span : spans) {
    if (span.to <= span.from) {
      continue;
    }
    rate.hours += hours_of(span.from, span.to);
    rate.incidents += std::ranges::count_if(
        times, [&](WallTime time) { return time >= span.from && time < span.to; });
  }
  return rate;
}

std::optional<Rate> baseline(std::span<const WallTime> times, WallTime at,
                             std::span<const Span> excluded) {
  const WallTime from = at - baseline_span;
  if (times.empty() || std::ranges::min(times) > from) {
    return std::nullopt;
  }
  // The window cut around the excluded spans, in time order.
  std::vector<Span> cuts(excluded.begin(), excluded.end());
  std::ranges::sort(cuts, {}, &Span::from);
  std::vector<Span> window;
  WallTime cursor = from;
  for (const Span& cut : cuts) {
    if (cut.to <= cursor || cut.from >= at) {
      continue;
    }
    if (cut.from > cursor) {
      window.push_back(Span{.from = cursor, .to = cut.from});
    }
    cursor = std::max(cursor, cut.to);
  }
  if (cursor < at) {
    window.push_back(Span{.from = cursor, .to = at});
  }
  return rate_in(times, window);
}

std::string_view to_string(EndReason reason) {
  return detail::enum_name(reason_names, reason);
}

std::optional<EndReason> parse_end_reason(std::string_view text) {
  return detail::enum_from_name<EndReason>(reason_names, text);
}

const store::MigrationSet& elimination_migrations() {
  static const store::MigrationSet set{.component = "elimination",
                                       .migrations = {
                                           {.version = 1, .name = "suspect_pause", .sql = R"sql(
CREATE TABLE suspect_pause(
  id INTEGER PRIMARY KEY,
  plan_id INTEGER NOT NULL,
  image TEXT NOT NULL,
  mode TEXT NOT NULL,
  started INTEGER NOT NULL,
  ends INTEGER NOT NULL,
  restored INTEGER,
  end_reason TEXT NOT NULL DEFAULT '',
  baseline_incidents INTEGER NOT NULL,
  baseline_hours REAL NOT NULL,
  pause_incidents INTEGER,
  pause_hours REAL);
CREATE INDEX suspect_pause_plan ON suspect_pause(plan_id);
CREATE TABLE suspect_pause_process(
  pause_id INTEGER NOT NULL,
  pid INTEGER NOT NULL,
  created INTEGER NOT NULL,
  count INTEGER NOT NULL,
  PRIMARY KEY(pause_id, pid, created));
CREATE TABLE suspect_pause_span(
  pause_id INTEGER NOT NULL,
  start INTEGER NOT NULL,
  stop INTEGER NOT NULL,
  PRIMARY KEY(pause_id, start));
)sql"},
                                       }};
  return set;
}

Result<std::int64_t> begin_pause(store::Database& db, const PauseRecord& record,
                                 std::span<const os::ProcessKey> processes) {
  Result<store::Statement> insert = db.prepare(
      "INSERT INTO suspect_pause(plan_id, image, mode, started, ends, baseline_incidents, "
      "baseline_hours) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
  if (!insert) {
    return tl::unexpected(insert.error());
  }
  insert->bind(1, record.plan.value())
      .bind(2, std::string_view{record.image})
      .bind(3, to_string(record.mode))
      .bind(4, store::to_unix_seconds(record.started))
      .bind(5, store::to_unix_seconds(record.ends))
      .bind(6, record.baseline.incidents)
      .bind(7, record.baseline.hours);
  if (Result<void> ran = insert->run(); !ran) {
    return tl::unexpected(ran.error());
  }
  const std::int64_t id = db.last_insert_id();
  Result<store::Statement> process = db.prepare(
      "INSERT INTO suspect_pause_process(pause_id, pid, created, count) VALUES(?1, ?2, ?3, 1)");
  if (!process) {
    return tl::unexpected(process.error());
  }
  for (const os::ProcessKey& key : processes) {
    process->bind(1, id).bind(2, key.pid).bind(3, nanoseconds_of(key.created));
    if (Result<void> ran = process->run(); !ran) {
      return tl::unexpected(ran.error());
    }
  }
  return id;
}

Result<std::vector<PausedProcess>> paused_processes(store::Database& db, std::int64_t id) {
  Result<store::Statement> query = db.prepare(
      "SELECT pid, created, count FROM suspect_pause_process WHERE pause_id = ?1 ORDER BY pid");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, id);
  std::vector<PausedProcess> processes;
  Result<bool> row = query->step();
  for (; row && *row; row = query->step()) {
    processes.push_back(PausedProcess{
        .process = os::ProcessKey{.pid = static_cast<std::uint32_t>(query->column_int(0)),
                                  .created = from_nanoseconds(query->column_int(1))},
        .count = static_cast<int>(query->column_int(2))});
  }
  if (!row) {
    return tl::unexpected(row.error());
  }
  return processes;
}

Result<void> forget_process(store::Database& db, std::int64_t id, const os::ProcessKey& process) {
  Result<store::Statement> remove = db.prepare(
      "DELETE FROM suspect_pause_process WHERE pause_id = ?1 AND pid = ?2 AND created = ?3");
  if (!remove) {
    return tl::unexpected(remove.error());
  }
  return remove->bind(1, id).bind(2, process.pid).bind(3, nanoseconds_of(process.created)).run();
}

Result<void> save_span(store::Database& db, std::int64_t id, WallTime from, WallTime to) {
  Result<store::Statement> upsert = db.prepare(
      "INSERT INTO suspect_pause_span(pause_id, start, stop) VALUES(?1, ?2, ?3) "
      "ON CONFLICT(pause_id, start) DO UPDATE SET stop = excluded.stop");
  if (!upsert) {
    return tl::unexpected(upsert.error());
  }
  return upsert->bind(1, id)
      .bind(2, store::to_unix_seconds(from))
      .bind(3, store::to_unix_seconds(to))
      .run();
}

Result<std::vector<Span>> spans_of(store::Database& db, std::int64_t id) {
  Result<store::Statement> query =
      db.prepare("SELECT start, stop FROM suspect_pause_span WHERE pause_id = ?1 ORDER BY start");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, id);
  std::vector<Span> spans;
  Result<bool> row = query->step();
  for (; row && *row; row = query->step()) {
    spans.push_back(Span{.from = store::from_unix_seconds(query->column_int(0)),
                         .to = store::from_unix_seconds(query->column_int(1))});
  }
  if (!row) {
    return tl::unexpected(row.error());
  }
  return spans;
}

Result<void> finish_pause(store::Database& db, std::int64_t id, WallTime restored, EndReason reason,
                          const Rate& during) {
  Result<store::Statement> update = db.prepare(
      "UPDATE suspect_pause SET restored = ?2, end_reason = ?3, pause_incidents = ?4, "
      "pause_hours = ?5 WHERE id = ?1");
  if (!update) {
    return tl::unexpected(update.error());
  }
  return update->bind(1, id)
      .bind(2, store::to_unix_seconds(restored))
      .bind(3, to_string(reason))
      .bind(4, during.incidents)
      .bind(5, during.hours)
      .run();
}

Result<std::optional<PauseRecord>> pause_of_plan(store::Database& db, PlanId plan) {
  return first_of(query_pauses(db, "WHERE plan_id = ?1", plan.value(), 1));
}

Result<std::vector<PauseRecord>> list_pauses(store::Database& db, std::size_t limit) {
  return query_pauses(db, "", std::nullopt, limit);
}

Result<std::optional<PauseRecord>> active_pause(store::Database& db) {
  return first_of(query_pauses(db, "WHERE restored IS NULL", std::nullopt, 1));
}

Result<std::vector<Span>> pause_periods(store::Database& db) {
  Result<store::Statement> query = db.prepare(
      "SELECT started, restored FROM suspect_pause WHERE restored IS NOT NULL ORDER BY started");
  if (!query) {
    return tl::unexpected(query.error());
  }
  std::vector<Span> periods;
  Result<bool> row = query->step();
  for (; row && *row; row = query->step()) {
    periods.push_back(Span{.from = store::from_unix_seconds(query->column_int(0)),
                           .to = store::from_unix_seconds(query->column_int(1))});
  }
  if (!row) {
    return tl::unexpected(row.error());
  }
  return periods;
}

}  // namespace myclean::elimination
