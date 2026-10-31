#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/time_series.hpp"

namespace myclean::store {

// The service database, %ProgramData%\MyClean\myclean.db, given the %ProgramData% folder. The
// service creates the folder and sets its ACL (SYSTEM and Administrators write, owner read).
[[nodiscard]] std::filesystem::path database_path(const std::filesystem::path& program_data);

// Retention deletes the rows of `table` whose `time_column` (Unix seconds) is older than
// `max_age`.
struct RetentionRule {
  std::string table;
  std::string time_column;
  std::chrono::seconds max_age{};
};

// MyClean's state in one SQLite database (docs/architecture.md section 5).
//
// Two write paths:
//  - write(): synchronous, committed before it returns. For action_log, undo_blob, quarantine,
//    plans and every other record that must survive a crash.
//  - enqueue(): buffered telemetry, event and inventory writes, committed together in one
//    transaction by tick() at most every 10 s, and by close() / the destructor on shutdown.
//
// Time series: record() feeds the in-memory ring of 1 s samples (last hour) and the minute
// aggregates; tick() writes closed minutes to ts_minute and aggregates closed hours into
// ts_hour. Retention runs from tick() every hour.
//
// All members are thread-safe. Time comes from the injected clock.
class Store {
 public:
  static constexpr std::chrono::seconds flush_interval{10};
  // A minute or hour is closed this long after it ends, so that its last samples are in.
  static constexpr std::chrono::seconds close_delay{2};
  static constexpr std::chrono::hours retention_interval{1};
  static constexpr std::chrono::days minute_retention{30};
  // Hour aggregates, events, findings and plans.
  static constexpr std::chrono::days history_retention{730};

  // Opens `path` (see Database::open), migrates the store's own schema, then each module's
  // `migrations` in order. Fails with the database name when the integrity check fails, and
  // with the migration's name when a migration fails.
  [[nodiscard]] static Result<std::unique_ptr<Store>> open(
      const std::filesystem::path& path, const os::IClock& clock,
      std::span<const MigrationSet> migrations = {});

  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  Store(Store&&) = delete;
  Store& operator=(Store&&) = delete;
  // Flushes buffered writes; call close() first to see a flush error.
  ~Store();

  // Runs `work` in one transaction, committed before this returns.
  [[nodiscard]] Result<void> write(const std::function<Result<void>(Database&)>& work);
  // Runs `work` on the database outside a transaction, for reads.
  [[nodiscard]] Result<void> read(const std::function<Result<void>(Database&)>& work);

  // Queues one statement with its parameters for the next flush.
  void enqueue(std::string sql, std::vector<Value> params);
  // Commits every queued statement in one transaction. On failure the queued writes are lost
  // and the error says how many.
  [[nodiscard]] Result<void> flush();

  // Adds a 1 s sample of `key`.
  void record(const SeriesKey& key, Sample sample);
  // The samples of `key` from the last hour, oldest first.
  [[nodiscard]] std::vector<Sample> raw(const SeriesKey& key) const;
  // Stored minute or hour aggregates of `key` starting in [from, to), oldest first.
  [[nodiscard]] Result<std::vector<Aggregate>> minutes(const SeriesKey& key, WallTime from,
                                                       WallTime to);
  [[nodiscard]] Result<std::vector<Aggregate>> hours(const SeriesKey& key, WallTime from,
                                                     WallTime to);

  // Adds a retention rule, typically for a module's event table. Refused for the tables that
  // retention never touches: action_log, undo_blob, schedule, snapshot and report.
  [[nodiscard]] Result<void> add_retention(RetentionRule rule);
  // Deletes every row older than its rule allows, in one transaction.
  [[nodiscard]] Result<void> run_retention();

  // Periodic work; call about once a second. Closes due minutes and hours, flushes when the
  // flush interval has passed, and runs retention when the retention interval has passed.
  [[nodiscard]] Result<void> tick();
  // Clean shutdown: flushes every buffered write.
  [[nodiscard]] Result<void> close();

 private:
  struct BufferedWrite {
    std::string sql;
    std::vector<Value> params;
  };

  Store(Database database, const os::IClock& clock);

  [[nodiscard]] Result<std::vector<Aggregate>> aggregates(std::string_view table,
                                                          const SeriesKey& key, WallTime from,
                                                          WallTime to);
  // Writes ts_hour rows for every closed hour before `until` not yet aggregated.
  [[nodiscard]] Result<void> aggregate_hours(WallTime until);

  const os::IClock& clock_;

  mutable std::mutex buffer_mutex_;  // guards the members below up to db_mutex_
  SeriesBuffer series_;
  std::vector<BufferedWrite> pending_;

  std::mutex db_mutex_;  // guards the members below
  Database db_;
  std::vector<RetentionRule> retention_;
  MonoTime last_flush_;
  std::optional<MonoTime> last_retention_;
  std::optional<WallTime> hours_until_;  // every hour before this is in ts_hour
};

}  // namespace myclean::store
