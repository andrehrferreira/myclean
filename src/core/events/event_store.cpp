#include "core/events/event_store.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/events/events.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

namespace myclean::events {

namespace {

constexpr std::int64_t seconds_per_minute = 60;

constexpr std::string_view insert_start = R"sql(
INSERT INTO proc_start(time, pid, parent_pid, parent_time, session, image, command_line_hash, signer)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8))sql";

constexpr std::string_view insert_minute = R"sql(
INSERT INTO net_minute(time, pid, process_time, image, protocol, remote_address, remote_port, sent,
                       received, first_seen, last_seen)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11))sql";

std::int64_t minute_of(WallTime time) {
  const std::int64_t seconds = store::to_unix_seconds(time);
  return seconds - (((seconds % seconds_per_minute) + seconds_per_minute) % seconds_per_minute);
}

store::Value value_of(const std::optional<WallTime>& time) {
  return time ? store::Value{store::to_unix_seconds(*time)} : store::Value{nullptr};
}

store::Value value_of(const std::optional<std::string>& text) {
  return text ? store::Value{*text} : store::Value{nullptr};
}

store::Value value_of(std::uint64_t count) {
  return store::Value{static_cast<std::int64_t>(count)};
}

}  // namespace

const store::MigrationSet& event_migrations() {
  static const store::MigrationSet set{
      .component = "events",
      .migrations = {
          {.version = 1, .name = "proc_start_and_net_minute", .sql = R"sql(
CREATE TABLE proc_start(
  time INTEGER NOT NULL,
  pid INTEGER NOT NULL,
  parent_pid INTEGER NOT NULL,
  parent_time INTEGER,
  session INTEGER NOT NULL,
  image TEXT NOT NULL,
  command_line_hash INTEGER,
  signer TEXT);
CREATE INDEX proc_start_time ON proc_start(time);
CREATE TABLE net_minute(
  time INTEGER NOT NULL,
  pid INTEGER NOT NULL,
  process_time INTEGER,
  image TEXT,
  protocol TEXT NOT NULL,
  remote_address TEXT NOT NULL,
  remote_port INTEGER NOT NULL,
  sent INTEGER NOT NULL,
  received INTEGER NOT NULL,
  first_seen INTEGER NOT NULL,
  last_seen INTEGER NOT NULL);
CREATE INDEX net_minute_time ON net_minute(time);
)sql"},
      }};
  return set;
}

Result<void> add_event_retention(store::Store& store) {
  Result<void> added = store.add_retention(
      {.table = "proc_start", .time_column = "time", .max_age = store::Store::history_retention});
  if (!added) {
    return added;
  }
  return store.add_retention(
      {.table = "net_minute", .time_column = "time", .max_age = store::Store::minute_retention});
}

std::int64_t command_line_hash(std::string_view command_line) {
  constexpr std::uint64_t offset_basis = 14'695'981'039'346'656'037ULL;
  constexpr std::uint64_t prime = 1'099'511'628'211ULL;
  std::uint64_t hash = offset_basis;
  for (const char character : command_line) {
    hash ^= static_cast<std::uint8_t>(character);
    hash *= prime;
  }
  return std::bit_cast<std::int64_t>(hash);
}

EventRecorder::EventRecorder(store::Store& store) : store_(&store) {}

void EventRecorder::on_start(const ProcessStart& event) {
  const std::optional<WallTime> parent_time =
      event.parent ? std::optional<WallTime>{event.parent->created} : std::nullopt;
  store_->enqueue(std::string(insert_start),
                  {store::to_unix_seconds(event.process.created), std::int64_t{event.process.pid},
                   std::int64_t{event.parent_pid}, value_of(parent_time),
                   std::int64_t{event.session}, event.image,
                   event.command_line ? store::Value{command_line_hash(*event.command_line)}
                                      : store::Value{nullptr},
                   value_of(event.signer)});
}

void EventRecorder::on_stop(const ProcessStop& /*event*/) {}

void EventRecorder::on_network(const NetworkTransfer& transfer) {
  const MinuteKey key{.minute = minute_of(transfer.time),
                      .pid = transfer.pid,
                      .created = transfer.process
                                     ? std::optional<WallTime>{transfer.process->process.created}
                                     : std::nullopt,
                      .protocol = transfer.protocol,
                      .remote_address = transfer.remote_address,
                      .remote_port = transfer.remote_port};
  const std::lock_guard lock(mutex_);
  auto [entry, added] = open_.try_emplace(key);
  MinuteTotals& totals = entry->second;
  if (added) {
    totals.image = transfer.process ? transfer.process->image : std::string{};
    totals.first_seen = transfer.time;
    totals.last_seen = transfer.time;
  } else {
    totals.first_seen = std::min(totals.first_seen, transfer.time);
    totals.last_seen = std::max(totals.last_seen, transfer.time);
  }
  (transfer.direction == Direction::send ? totals.sent : totals.received) += transfer.bytes;
}

void EventRecorder::close_minutes(WallTime now) {
  const std::lock_guard lock(mutex_);
  write_minutes_before(minute_of(now - store::Store::close_delay));
}

void EventRecorder::close_all() {
  const std::lock_guard lock(mutex_);
  write_minutes_before(std::numeric_limits<std::int64_t>::max());
}

void EventRecorder::write_minutes_before(std::int64_t end) {
  for (auto entry = open_.begin(); entry != open_.end();) {
    const auto& [key, totals] = *entry;
    if (key.minute >= end) {
      ++entry;
      continue;
    }
    store_->enqueue(
        std::string(insert_minute),
        {key.minute, std::int64_t{key.pid}, value_of(key.created),
         totals.image.empty() ? store::Value{nullptr} : store::Value{totals.image},
         std::string(to_string(key.protocol)), to_string(key.remote_address),
         std::int64_t{key.remote_port}, value_of(totals.sent), value_of(totals.received),
         store::to_unix_seconds(totals.first_seen), store::to_unix_seconds(totals.last_seen)});
    entry = open_.erase(entry);
  }
}

Result<std::vector<ProcessNetwork>> network_by_process(store::Store& store, WallTime from,
                                                       WallTime to) {
  std::vector<ProcessNetwork> totals;
  Result<void> read = store.read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> statement = db.prepare(R"sql(
SELECT pid, process_time, IFNULL(image, ''), SUM(sent), SUM(received) FROM net_minute
WHERE time >= ?1 AND time < ?2
GROUP BY pid, process_time, image
ORDER BY SUM(sent) + SUM(received) DESC)sql");
    if (!statement) {
      return tl::unexpected(statement.error());
    }
    statement->bind(1, store::to_unix_seconds(from)).bind(2, store::to_unix_seconds(to));
    for (;;) {
      Result<bool> row = statement->step();
      if (!row) {
        return tl::unexpected(row.error());
      }
      if (!*row) {
        return {};
      }
      totals.push_back({.pid = static_cast<std::uint32_t>(statement->column_int(0)),
                        .created = statement->is_null(1)
                                       ? std::nullopt
                                       : std::optional<WallTime>{store::from_unix_seconds(
                                             statement->column_int(1))},
                        .image = statement->column_text(2),
                        .sent = static_cast<std::uint64_t>(statement->column_int(3)),
                        .received = static_cast<std::uint64_t>(statement->column_int(4))});
    }
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return totals;
}

}  // namespace myclean::events
