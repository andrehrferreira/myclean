#include "core/governing/records.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/actions/audit_log.hpp"
#include "core/governing/keys.hpp"
#include "core/governor/commands.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/pressure.hpp"
#include "core/governor/reason.hpp"
#include "core/governor/recovery.hpp"
#include "core/model/actor.hpp"
#include "core/model/enum_names.hpp"
#include "core/model/ids.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/jobs.hpp"
#include "core/rules/schema.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

namespace myclean::governing {

namespace {

constexpr std::array<std::string_view, 16> kind_names = {"apply_step",
                                                         "roll_back",
                                                         "resume",
                                                         "kill",
                                                         "notify",
                                                         "add_to_allow_list",
                                                         "place_job",
                                                         "job_released",
                                                         "job_adopted",
                                                         "job_limits",
                                                         "notification_limit",
                                                         "job_memory_limit",
                                                         "commit_failure_exit",
                                                         "untrusted_job",
                                                         "game_detected",
                                                         "game_mode"};

// The action_log event of every governor intervention (docs/architecture.md section 6).
constexpr std::string_view governor_step_event = "GovernorStep";

constexpr std::string_view insert_event = R"sql(
INSERT INTO governor_event(time, kind, root_pid, root_created_ns, root_image, class, step, reason,
                           actor, members, cpu_percent, available, commit_bytes, commit_limit,
                           hard_faults, detail)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16))sql";

constexpr std::string_view select_events = R"sql(
SELECT id, time, kind, root_pid, root_created_ns, root_image, class, step, reason, actor, members,
       cpu_percent, available, commit_bytes, commit_limit, hard_faults, detail
FROM governor_event WHERE time >= ?1 ORDER BY id DESC LIMIT ?2)sql";

std::int64_t bytes_value(Bytes bytes) {
  return static_cast<std::int64_t>(bytes.count());
}

// "cpu=97.0% available=7.50 GiB commit=120.00/150.00 GiB hard_faults=12/s"
std::string metrics_text(const std::optional<governor::PressureSample>& metrics) {
  if (!metrics) {
    return {};
  }
  constexpr double gib = 1024.0 * 1024.0 * 1024.0;
  return std::format(
      "cpu={:.1f}% available={:.2f} GiB commit={:.2f}/{:.2f} GiB hard_faults={:.0f}/s",
      metrics->cpu_percent, static_cast<double>(metrics->available.count()) / gib,
      static_cast<double>(metrics->commit.count()) / gib,
      static_cast<double>(metrics->commit_limit.count()) / gib, metrics->hard_faults_per_second);
}

// The GovernorStep entry of an event: outcome its kind, the step and tree in detail.
actions::LogEntry log_entry(const GovernorEvent& event) {
  std::string detail;
  if (event.step) {
    detail = std::string(governor::to_string(*event.step)) + " ";
  }
  detail += event.root_image.empty() ? std::string("(no image)") : event.root_image;
  if (event.root) {
    detail += std::format(" pid {}", event.root->pid);
  }
  if (event.process_class) {
    detail += std::format(" class {}", rules::to_string(*event.process_class));
  }
  detail += std::format(", {} processes", event.members);
  if (!event.detail.empty()) {
    detail += ": " + event.detail;
  }
  return actions::LogEntry{.time = event.time,
                           .plan = PlanId{0},
                           .item = std::nullopt,
                           .actor = event.actor.to_string(),
                           .event = std::string(governor_step_event),
                           .outcome = std::string(to_string(event.kind)),
                           .reason = std::string(governor::to_string(event.reason)),
                           .detail = std::move(detail),
                           .metrics = metrics_text(event.metrics)};
}

Result<void> insert(store::Database& db, const GovernorEvent& event) {
  Result<store::Statement> statement = db.prepare(insert_event);
  if (!statement) {
    return tl::unexpected(statement.error());
  }
  statement->bind(1, store::to_unix_seconds(event.time))
      .bind(2, to_string(event.kind))
      .bind(5, event.root_image)
      .bind(8, governor::to_string(event.reason))
      .bind(9, event.actor.to_string())
      .bind(10, static_cast<std::int64_t>(event.members))
      .bind(16, event.detail);
  if (event.root) {
    statement->bind(3, event.root->pid).bind(4, to_nanoseconds(event.root->created));
  } else {
    statement->bind(3, nullptr).bind(4, nullptr);
  }
  if (event.process_class) {
    statement->bind(6, rules::to_string(*event.process_class));
  } else {
    statement->bind(6, nullptr);
  }
  if (event.step) {
    statement->bind(7, governor::to_string(*event.step));
  } else {
    statement->bind(7, nullptr);
  }
  if (event.metrics) {
    statement->bind(11, event.metrics->cpu_percent)
        .bind(12, bytes_value(event.metrics->available))
        .bind(13, bytes_value(event.metrics->commit))
        .bind(14, bytes_value(event.metrics->commit_limit))
        .bind(15, event.metrics->hard_faults_per_second);
  } else {
    for (int column = 11; column <= 15; ++column) {
      statement->bind(column, nullptr);
    }
  }
  return statement->run();
}

Bytes bytes_column(const store::Statement& statement, int column) {
  return Bytes{static_cast<std::uint64_t>(statement.column_int(column))};
}

GovernorEvent read_event(const store::Statement& row) {
  GovernorEvent event{
      .id = row.column_int(0),
      .time = store::from_unix_seconds(row.column_int(1)),
      .kind = parse_event_kind(row.column_text(2)).value_or(EventKind::apply_step),
      .root_image = row.column_text(5),
      .reason = governor::parse_reason(row.column_text(8)).value_or(governor::Reason::policy),
      .actor = Actor::parse(row.column_text(9)).value_or(Actor::governor()),
      .members = static_cast<std::size_t>(row.column_int(10)),
      .detail = row.column_text(16)};
  if (!row.is_null(3) && !row.is_null(4)) {
    event.root = governor::ProcessKey{.pid = static_cast<std::uint32_t>(row.column_int(3)),
                                      .created = from_nanoseconds(row.column_int(4))};
  }
  if (!row.is_null(6)) {
    event.process_class = rules::parse_process_class(row.column_text(6));
  }
  if (!row.is_null(7)) {
    event.step = governor::parse_step(row.column_text(7));
  }
  if (!row.is_null(11)) {
    event.metrics = governor::PressureSample{.cpu_percent = row.column_double(11),
                                             .available = bytes_column(row, 12),
                                             .commit = bytes_column(row, 13),
                                             .commit_limit = bytes_column(row, 14),
                                             .hard_faults_per_second = row.column_double(15)};
  }
  return event;
}

// Runs `sql` with the key of `process` as ?1 (pid) and ?2 (creation time).
Result<void> run_keyed(store::Database& db, std::string_view sql,
                       const governor::ProcessKey& process) {
  Result<store::Statement> statement = db.prepare(sql);
  if (!statement) {
    return tl::unexpected(statement.error());
  }
  return statement->bind(1, process.pid).bind(2, to_nanoseconds(process.created)).run();
}

}  // namespace

const store::MigrationSet& governor_migrations() {
  static const store::MigrationSet set{
      .component = "governor",
      .migrations = {{.version = 1, .name = "governor_tables", .sql = R"sql(
CREATE TABLE governor_event(
  id INTEGER PRIMARY KEY,
  time INTEGER NOT NULL,
  kind TEXT NOT NULL,
  root_pid INTEGER,
  root_created_ns INTEGER,
  root_image TEXT NOT NULL DEFAULT '',
  class TEXT,
  step TEXT,
  reason TEXT NOT NULL,
  actor TEXT NOT NULL,
  members INTEGER NOT NULL DEFAULT 0,
  cpu_percent REAL,
  available INTEGER,
  commit_bytes INTEGER,
  commit_limit INTEGER,
  hard_faults REAL,
  detail TEXT NOT NULL DEFAULT '');
CREATE INDEX governor_event_time ON governor_event(time);
CREATE TABLE governor_job(
  job_class TEXT NOT NULL,
  nonce TEXT NOT NULL,
  role TEXT NOT NULL,
  class TEXT NOT NULL,
  root_pid INTEGER,
  root_created_ns INTEGER,
  root_image TEXT NOT NULL DEFAULT '',
  created INTEGER NOT NULL,
  PRIMARY KEY(job_class, nonce));
CREATE TABLE governor_suspend(
  pid INTEGER NOT NULL,
  created_ns INTEGER NOT NULL,
  count INTEGER NOT NULL,
  PRIMARY KEY(pid, created_ns));
CREATE TABLE governor_allow(
  image TEXT NOT NULL PRIMARY KEY COLLATE NOCASE,
  added INTEGER NOT NULL);
)sql"}}};
  return set;
}

store::RetentionRule governor_retention() {
  return {
      .table = "governor_event", .time_column = "time", .max_age = store::Store::history_retention};
}

std::string_view to_string(EventKind kind) {
  return detail::enum_name(kind_names, kind);
}

std::optional<EventKind> parse_event_kind(std::string_view text) {
  return detail::enum_from_name<EventKind>(kind_names, text);
}

std::optional<EventKind> event_kind(governor::CommandKind kind) {
  switch (kind) {
    case governor::CommandKind::apply_step:
      return EventKind::apply_step;
    case governor::CommandKind::roll_back:
      return EventKind::roll_back;
    case governor::CommandKind::resume:
      return EventKind::resume;
    case governor::CommandKind::kill:
      return EventKind::kill;
    case governor::CommandKind::notify:
      return EventKind::notify;
    case governor::CommandKind::add_to_allow_list:
      return EventKind::add_to_allow_list;
    case governor::CommandKind::persist_suspend:
      return std::nullopt;
  }
  return std::nullopt;
}

Result<std::int64_t> save_event(store::Store& store, const GovernorEvent& event, bool audit) {
  std::int64_t id = 0;
  Result<void> written = store.write([&](store::Database& db) -> Result<void> {
    if (Result<void> inserted = insert(db, event); !inserted) {
      return inserted;
    }
    id = db.last_insert_id();
    if (!audit) {
      return {};
    }
    return actions::append_log(db, log_entry(event)).transform([](std::int64_t) {});
  });
  if (!written) {
    return tl::unexpected(written.error());
  }
  return id;
}

Result<std::vector<GovernorEvent>> load_events(store::Store& store, WallTime since,
                                               std::size_t limit) {
  std::vector<GovernorEvent> events;
  Result<void> read = store.read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> query = db.prepare(select_events);
    if (!query) {
      return tl::unexpected(query.error());
    }
    query->bind(1, store::to_unix_seconds(since)).bind(2, static_cast<std::int64_t>(limit));
    Result<bool> row = query->step();
    for (; row && *row; row = query->step()) {
      events.push_back(read_event(*query));
    }
    return row.transform([](bool) {});
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return events;
}

Result<void> save_job(store::Store& store, const JobRecord& job) {
  return store.write([&](store::Database& db) -> Result<void> {
    Result<store::Statement> statement = db.prepare(
        "INSERT OR REPLACE INTO governor_job(job_class, nonce, role, class, root_pid, "
        "root_created_ns, root_image, created) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
    if (!statement) {
      return tl::unexpected(statement.error());
    }
    statement->bind(1, job.ref.job_class)
        .bind(2, job.ref.nonce)
        .bind(3, governor::to_string(job.role))
        .bind(4, rules::to_string(job.process_class))
        .bind(7, job.root_image)
        .bind(8, store::to_unix_seconds(job.created));
    if (job.root) {
      statement->bind(5, job.root->pid).bind(6, to_nanoseconds(job.root->created));
    } else {
      statement->bind(5, nullptr).bind(6, nullptr);
    }
    return statement->run();
  });
}

Result<void> forget_job(store::Store& store, const os::JobRef& job) {
  return store.write([&](store::Database& db) -> Result<void> {
    Result<store::Statement> statement =
        db.prepare("DELETE FROM governor_job WHERE job_class = ?1 AND nonce = ?2");
    if (!statement) {
      return tl::unexpected(statement.error());
    }
    return statement->bind(1, job.job_class).bind(2, job.nonce).run();
  });
}

Result<std::vector<JobRecord>> load_jobs(store::Store& store) {
  std::vector<JobRecord> jobs;
  Result<void> read = store.read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> query = db.prepare(
        "SELECT job_class, nonce, role, class, root_pid, root_created_ns, root_image, "
        "created FROM governor_job ORDER BY created, job_class, nonce");
    if (!query) {
      return tl::unexpected(query.error());
    }
    Result<bool> row = query->step();
    for (; row && *row; row = query->step()) {
      const std::optional<governor::JobRole> role = governor::parse_job_role(query->column_text(2));
      const std::optional<rules::ProcessClass> process_class =
          rules::parse_process_class(query->column_text(3));
      if (!role || !process_class) {
        continue;
      }
      JobRecord job{.ref = {.job_class = query->column_text(0), .nonce = query->column_text(1)},
                    .role = *role,
                    .process_class = *process_class,
                    .root_image = query->column_text(6),
                    .created = store::from_unix_seconds(query->column_int(7))};
      if (!query->is_null(4) && !query->is_null(5)) {
        job.root = governor::ProcessKey{.pid = static_cast<std::uint32_t>(query->column_int(4)),
                                        .created = from_nanoseconds(query->column_int(5))};
      }
      jobs.push_back(std::move(job));
    }
    return row.transform([](bool) {});
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return jobs;
}

Result<void> add_suspends(store::Store& store, std::span<const governor::ProcessKey> processes) {
  return store.write([&](store::Database& db) -> Result<void> {
    for (const governor::ProcessKey& process : processes) {
      if (Result<void> added =
              run_keyed(db,
                        "INSERT INTO governor_suspend(pid, created_ns, count) VALUES(?1, ?2, 1) "
                        "ON CONFLICT(pid, created_ns) DO UPDATE SET count = count + 1",
                        process);
          !added) {
        return added;
      }
    }
    return {};
  });
}

Result<void> remove_suspends(store::Store& store, const governor::ProcessKey& process, int times) {
  return store.write([&](store::Database& db) -> Result<void> {
    Result<store::Statement> statement = db.prepare(
        "UPDATE governor_suspend SET count = count - ?3 WHERE pid = ?1 AND created_ns = ?2");
    if (!statement) {
      return tl::unexpected(statement.error());
    }
    if (Result<void> updated = statement->bind(1, process.pid)
                                   .bind(2, to_nanoseconds(process.created))
                                   .bind(3, times)
                                   .run();
        !updated) {
      return updated;
    }
    return run_keyed(
        db, "DELETE FROM governor_suspend WHERE pid = ?1 AND created_ns = ?2 AND count <= 0",
        process);
  });
}

Result<void> forget_suspend(store::Store& store, const governor::ProcessKey& process) {
  return store.write([&](store::Database& db) {
    return run_keyed(db, "DELETE FROM governor_suspend WHERE pid = ?1 AND created_ns = ?2",
                     process);
  });
}

Result<std::vector<governor::PersistedSuspend>> load_suspends(store::Store& store) {
  std::vector<governor::PersistedSuspend> suspends;
  Result<void> read = store.read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> query =
        db.prepare("SELECT pid, created_ns, count FROM governor_suspend ORDER BY pid, created_ns");
    if (!query) {
      return tl::unexpected(query.error());
    }
    Result<bool> row = query->step();
    for (; row && *row; row = query->step()) {
      suspends.push_back(governor::PersistedSuspend{
          .process = {.pid = static_cast<std::uint32_t>(query->column_int(0)),
                      .created = from_nanoseconds(query->column_int(1))},
          .count = static_cast<int>(query->column_int(2))});
    }
    return row.transform([](bool) {});
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return suspends;
}

Result<void> add_allowed(store::Store& store, std::string_view image, WallTime now) {
  return store.write([&](store::Database& db) -> Result<void> {
    Result<store::Statement> statement =
        db.prepare("INSERT OR IGNORE INTO governor_allow(image, added) VALUES(?1, ?2)");
    if (!statement) {
      return tl::unexpected(statement.error());
    }
    return statement->bind(1, image).bind(2, store::to_unix_seconds(now)).run();
  });
}

Result<std::vector<std::string>> load_allowed(store::Store& store) {
  std::vector<std::string> images;
  Result<void> read = store.read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> query = db.prepare("SELECT image FROM governor_allow ORDER BY added");
    if (!query) {
      return tl::unexpected(query.error());
    }
    Result<bool> row = query->step();
    for (; row && *row; row = query->step()) {
      images.push_back(query->column_text(0));
    }
    return row.transform([](bool) {});
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return images;
}

void Recorder::record(GovernorEvent event, bool audit) {
  event.time = clock_->now();
  if (!event.metrics) {
    event.metrics = metrics_;
  }
  Result<std::int64_t> saved = save_event(*store_, event, audit);
  if (saved) {
    event.id = *saved;
    problem_.reset();
  } else {
    problem_ = "governor_event: " + saved.error().message;
  }
  recent_.push_front(std::move(event));
  if (recent_.size() > kept) {
    recent_.pop_back();
  }
}

std::vector<GovernorEvent> Recorder::recent(std::size_t limit) const {
  std::vector<GovernorEvent> events;
  for (const GovernorEvent& event : recent_) {
    if (events.size() >= limit) {
      break;
    }
    events.push_back(event);
  }
  return events;
}

}  // namespace myclean::governing
