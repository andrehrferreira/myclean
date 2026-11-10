#include "core/input/incident_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/input/classifier.hpp"
#include "core/input/incident.hpp"
#include "core/model/result.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

namespace myclean::input {

namespace {

using nlohmann::json;

constexpr std::string_view insert_sql = R"sql(
INSERT INTO input_incident(time, chord, latency_ms, capped, owner_unanswered_ms, input_lag_ms,
  foreground_pid, foreground_image, owner_pid, owner_image, holder_pid, holder_image,
  viewer_pid, viewer_image, formats, hung, suspects, cpu_percent, memory_load_percent)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19))sql";

constexpr std::string_view select_sql = R"sql(
SELECT id, time, chord, latency_ms, capped, owner_unanswered_ms, input_lag_ms,
  foreground_pid, foreground_image, owner_pid, owner_image, holder_pid, holder_image,
  viewer_pid, viewer_image, formats, hung, suspects, cpu_percent, memory_load_percent
FROM input_incident ORDER BY time DESC, id DESC)sql";

void bind_process(store::Statement& statement, int index,
                  const std::optional<ProcessRef>& process) {
  if (process) {
    statement.bind(index, process->pid).bind(index + 1, std::string_view{process->image});
  } else {
    statement.bind(index, nullptr).bind(index + 1, nullptr);
  }
}

std::optional<ProcessRef> read_process(const store::Statement& row, int column) {
  if (row.is_null(column)) {
    return std::nullopt;
  }
  return ProcessRef{.pid = static_cast<std::uint32_t>(row.column_int(column)),
                    .image = row.column_text(column + 1)};
}

std::string formats_json(const std::vector<ClipboardFormat>& formats) {
  json list = json::array();
  for (const ClipboardFormat& format : formats) {
    list.push_back({{"id", format.id}, {"name", format.name}, {"synthesized", format.synthesized}});
  }
  return list.dump();
}

std::string hung_json(const std::vector<ProcessRef>& hung) {
  json list = json::array();
  for (const ProcessRef& process : hung) {
    list.push_back({{"pid", process.pid}, {"image", process.image}});
  }
  return list.dump();
}

// Reads the JSON lists back; throws json::exception on malformed text.
void read_lists(const store::Statement& row, Incident& incident) {
  for (const json& format : json::parse(row.column_text(15))) {
    incident.formats.push_back({.id = format.at("id").get<std::uint32_t>(),
                                .name = format.at("name").get<std::string>(),
                                .synthesized = format.at("synthesized").get<bool>()});
  }
  for (const json& process : json::parse(row.column_text(16))) {
    incident.hung.push_back({.pid = process.at("pid").get<std::uint32_t>(),
                             .image = process.at("image").get<std::string>()});
  }
  incident.suspects = json::parse(row.column_text(17)).get<std::vector<std::string>>();
}

Result<StoredIncident> read_row(const store::Statement& row) {
  const std::int64_t id = row.column_int(0);
  const std::optional<Chord> chord = parse_chord(row.column_text(2));
  if (!chord) {
    return fail(Errc::invalid_data, "input_incident " + std::to_string(id) +
                                        " has an unknown chord '" + row.column_text(2) + "'");
  }
  Incident incident{.chord = *chord,
                    .time = store::from_unix_seconds(row.column_int(1)),
                    .latency = std::chrono::milliseconds{row.column_int(3)},
                    .capped = row.column_int(4) != 0,
                    .input_lag = std::chrono::milliseconds{row.column_int(6)},
                    .foreground = read_process(row, 7),
                    .owner = read_process(row, 9),
                    .holder = read_process(row, 11),
                    .viewer = read_process(row, 13)};
  if (!row.is_null(5)) {
    incident.owner_unanswered = std::chrono::milliseconds{row.column_int(5)};
  }
  if (!row.is_null(18)) {
    incident.cpu_percent = row.column_double(18);
  }
  if (!row.is_null(19)) {
    incident.memory_load_percent = static_cast<int>(row.column_int(19));
  }
  try {
    read_lists(row, incident);
  } catch (const json::exception& error) {
    return fail(Errc::invalid_data,
                "input_incident " + std::to_string(id) + " has malformed lists: " + error.what());
  }
  Classification classification = classify(incident);
  return StoredIncident{
      .id = id, .incident = std::move(incident), .classification = std::move(classification)};
}

}  // namespace

const store::MigrationSet& input_migrations() {
  static const store::MigrationSet set{.component = "input",
                                       .migrations = {
                                           {.version = 1, .name = "input_incident", .sql = R"sql(
CREATE TABLE input_incident(
  id INTEGER PRIMARY KEY,
  time INTEGER NOT NULL,
  chord TEXT NOT NULL,
  latency_ms INTEGER NOT NULL,
  capped INTEGER NOT NULL,
  owner_unanswered_ms INTEGER,
  input_lag_ms INTEGER NOT NULL,
  foreground_pid INTEGER,
  foreground_image TEXT,
  owner_pid INTEGER,
  owner_image TEXT,
  holder_pid INTEGER,
  holder_image TEXT,
  viewer_pid INTEGER,
  viewer_image TEXT,
  formats TEXT NOT NULL,
  hung TEXT NOT NULL,
  suspects TEXT NOT NULL,
  cpu_percent REAL,
  memory_load_percent INTEGER);
CREATE INDEX input_incident_time ON input_incident(time);
)sql"},
                                       }};
  return set;
}

store::RetentionRule input_retention() {
  return {
      .table = "input_incident", .time_column = "time", .max_age = store::Store::history_retention};
}

Result<std::int64_t> save_incident(store::Store& store, const Incident& incident) {
  std::int64_t id = 0;
  const Result<void> written = store.write([&](store::Database& db) -> Result<void> {
    Result<store::Statement> statement = db.prepare(insert_sql);
    if (!statement) {
      return tl::unexpected(statement.error());
    }
    statement->bind(1, store::to_unix_seconds(incident.time))
        .bind(2, to_string(incident.chord))
        .bind(3, incident.latency.count())
        .bind(4, incident.capped ? 1 : 0)
        .bind(6, incident.input_lag.count())
        .bind(15, std::string_view{formats_json(incident.formats)})
        .bind(16, std::string_view{hung_json(incident.hung)})
        .bind(17, std::string_view{json(incident.suspects).dump()});
    if (incident.owner_unanswered) {
      statement->bind(5, incident.owner_unanswered->count());
    } else {
      statement->bind(5, nullptr);
    }
    bind_process(*statement, 7, incident.foreground);
    bind_process(*statement, 9, incident.owner);
    bind_process(*statement, 11, incident.holder);
    bind_process(*statement, 13, incident.viewer);
    if (incident.cpu_percent) {
      statement->bind(18, *incident.cpu_percent);
    } else {
      statement->bind(18, nullptr);
    }
    if (incident.memory_load_percent) {
      statement->bind(19, *incident.memory_load_percent);
    } else {
      statement->bind(19, nullptr);
    }
    Result<void> ran = statement->run();
    if (!ran) {
      return ran;
    }
    id = db.last_insert_id();
    return {};
  });
  if (!written) {
    return tl::unexpected(written.error());
  }
  return id;
}

Result<std::vector<FreezeGroup>> freeze_summary(store::Store& store) {
  std::vector<StoredIncident> incidents;
  const Result<void> read = store.read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> statement = db.prepare(select_sql);
    if (!statement) {
      return tl::unexpected(statement.error());
    }
    for (;;) {
      Result<bool> row = statement->step();
      if (!row) {
        return tl::unexpected(row.error());
      }
      if (!*row) {
        return {};
      }
      Result<StoredIncident> incident = read_row(*statement);
      if (!incident) {
        return tl::unexpected(incident.error());
      }
      incidents.push_back(*std::move(incident));
    }
  });
  if (!read) {
    return tl::unexpected(read.error());
  }

  // Incidents arrive newest first, so each group's first incident is its latest and groups are
  // created in order of their latest incident.
  std::vector<FreezeGroup> groups;
  for (StoredIncident& stored : incidents) {
    const Classification& classification = stored.classification;
    std::optional<std::string> process;
    if (classification.process) {
      process = classification.process->image;
    }
    auto group = std::ranges::find_if(groups, [&](const FreezeGroup& candidate) {
      return candidate.mechanism == classification.mechanism && candidate.process == process;
    });
    if (group == groups.end()) {
      groups.push_back({.mechanism = classification.mechanism,
                        .process = std::move(process),
                        .latest = stored.incident.time});
      group = std::prev(groups.end());
    }
    group->incidents.push_back(std::move(stored));
  }
  return groups;
}

Result<std::vector<WallTime>> incident_times(store::Store& store) {
  std::vector<WallTime> times;
  const Result<void> read = store.read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> statement =
        db.prepare("SELECT time FROM input_incident ORDER BY time, id");
    if (!statement) {
      return tl::unexpected(statement.error());
    }
    Result<bool> row = statement->step();
    for (; row && *row; row = statement->step()) {
      times.push_back(store::from_unix_seconds(statement->column_int(0)));
    }
    return row ? Result<void>{} : tl::unexpected(row.error());
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return times;
}

}  // namespace myclean::input
