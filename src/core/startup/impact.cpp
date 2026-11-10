#include "core/startup/impact.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/startup/startup.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

namespace myclean::startup {
namespace {

std::string lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

// The lower-case file name of `path`, '\' or '/' separated.
std::string file_name(std::string_view path) {
  const std::size_t slash = path.find_last_of("\\/");
  return lower(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

// A process instance: PIDs are reused, a PID with its start (whole seconds, as the history
// records it) is not.
using Instance = std::pair<std::uint32_t, std::int64_t>;

std::int64_t seconds_of(WallTime time) {
  return std::chrono::floor<std::chrono::seconds>(time).time_since_epoch().count();
}

template <typename T>
T median(std::vector<T> values) {
  std::ranges::sort(values);
  const std::size_t middle = values.size() / 2;
  if (values.size() % 2 == 1) {
    return values[middle];
  }
  return static_cast<T>((values[middle - 1] + values[middle]) / 2);
}

}  // namespace

std::string command_image(std::string_view command) {
  const std::size_t start = command.find_first_not_of(" \t");
  if (start == std::string_view::npos) {
    return {};
  }
  command.remove_prefix(start);
  if (command.front() == '"') {
    const std::size_t close = command.find('"', 1);
    return file_name(command.substr(1, close == std::string_view::npos ? close : close - 1));
  }
  const std::string lowered = lower(command);
  const std::size_t exe = lowered.find(".exe");
  if (exe != std::string::npos) {
    return file_name(command.substr(0, exe + 4));
  }
  return file_name(command.substr(0, command.find_first_of(" \t")));
}

std::vector<ImageImpact> logon_impact(WallTime logon, std::span<const Start> starts,
                                      std::span<const Usage> baseline, std::span<const Usage> end) {
  const WallTime window_end = logon + logon_window;
  // The processes started in the window and, for each, the image its chain of parents began
  // with.
  std::map<Instance, const Start*> started;
  for (const Start& start : starts) {
    if (start.time >= logon && start.time < window_end) {
      started.emplace(Instance{start.pid, seconds_of(start.time)}, &start);
    }
  }
  const auto root_image = [&](const Start* start) {
    // Bounded: a chain is never longer than the starts in the window.
    for (std::size_t step = 0; step <= started.size() && start->parent_time; ++step) {
      const auto parent = started.find({start->parent_pid, seconds_of(*start->parent_time)});
      if (parent == started.end()) {
        break;
      }
      start = parent->second;
    }
    return file_name(start->image);
  };
  // The start of a sampled process: the history keeps whole seconds and its event may fall in
  // the second next to the creation time.
  const auto start_of = [&](std::uint32_t pid, WallTime created) -> const Start* {
    const std::int64_t second = seconds_of(created);
    for (const std::int64_t near : {second, second - 1, second + 1}) {
      if (const auto found = started.find({pid, near}); found != started.end()) {
        return found->second;
      }
    }
    return nullptr;
  };
  std::map<Instance, const Usage*> before;
  for (const Usage& usage : baseline) {
    before.emplace(Instance{usage.pid, seconds_of(usage.created)}, &usage);
  }
  std::map<std::string, ImageImpact> by_image;
  const auto add = [&](const std::string& image, std::chrono::nanoseconds cpu,
                       std::uint64_t io_bytes) {
    ImageImpact& impact = by_image[image];
    impact.image = image;
    impact.cpu_seconds += std::chrono::duration<double>(cpu).count();
    impact.io_bytes += io_bytes;
  };
  for (const Usage& usage : end) {
    const Instance instance{usage.pid, seconds_of(usage.created)};
    if (const Start* start = start_of(usage.pid, usage.created); start != nullptr) {
      add(root_image(start), usage.cpu, usage.io_bytes);
    } else if (const auto earlier = before.find(instance);
               earlier != before.end() && usage.created < logon) {
      add(file_name(usage.image), std::max(usage.cpu - earlier->second->cpu, {}),
          usage.io_bytes >= earlier->second->io_bytes ? usage.io_bytes - earlier->second->io_bytes
                                                      : 0);
    }
  }
  std::vector<ImageImpact> impacts;
  impacts.reserve(by_image.size());
  for (auto& [image, impact] : by_image) {
    impacts.push_back(std::move(impact));
  }
  return impacts;
}

std::string_view to_string(ImpactSource source) {
  return source == ImpactSource::myclean ? "myclean" : "diagnostics";
}

std::vector<std::optional<ItemImpact>> item_impacts(
    std::span<const Item> items, std::span<const std::vector<ImageImpact>> history,
    std::span<const BootDelay> delays) {
  std::vector<std::optional<ItemImpact>> impacts;
  impacts.reserve(items.size());
  for (const Item& item : items) {
    const std::string image = command_image(item.command);
    std::vector<double> cpu;
    std::vector<std::uint64_t> io;
    bool seen = false;
    for (const std::vector<ImageImpact>& logon : history) {
      const auto found = std::ranges::find(logon, image, &ImageImpact::image);
      seen = seen || found != logon.end();
      cpu.push_back(found == logon.end() ? 0.0 : found->cpu_seconds);
      io.push_back(found == logon.end() ? 0 : found->io_bytes);
    }
    if (seen) {
      impacts.emplace_back(ItemImpact{.source = ImpactSource::myclean,
                                      .cpu_seconds = median(std::move(cpu)),
                                      .io_bytes = median(std::move(io)),
                                      .logons = static_cast<std::uint32_t>(history.size())});
      continue;
    }
    std::optional<ItemImpact> delay;
    for (const BootDelay& boot : delays) {
      if (boot.image == image && (!delay || boot.delay_seconds > delay->delay_seconds)) {
        delay =
            ItemImpact{.source = ImpactSource::diagnostics, .delay_seconds = boot.delay_seconds};
      }
    }
    impacts.push_back(delay);
  }
  return impacts;
}

const store::MigrationSet& impact_migrations() {
  static const store::MigrationSet set{.component = "startup",
                                       .migrations = {
                                           {.version = 1, .name = "logon_impact", .sql = R"sql(
CREATE TABLE logon_impact(
  logon INTEGER NOT NULL,
  image TEXT NOT NULL,
  cpu_seconds REAL NOT NULL,
  io_bytes INTEGER NOT NULL,
  PRIMARY KEY (logon, image)
);
CREATE TABLE logon_recorded(
  logon INTEGER PRIMARY KEY
);
)sql"},
                                           {.version = 2, .name = "boot_time", .sql = R"sql(
CREATE TABLE boot_time(
  start INTEGER PRIMARY KEY,
  boot_ms INTEGER NOT NULL,
  main_path_ms INTEGER NOT NULL,
  post_boot_ms INTEGER NOT NULL,
  logon_ms INTEGER NOT NULL
);
)sql"},
                                       }};
  return set;
}

Result<void> record_logon(store::Store& store, WallTime logon,
                          std::span<const ImageImpact> impact) {
  const std::int64_t seconds = store::to_unix_seconds(logon);
  return store.write([&](store::Database& db) -> Result<void> {
    Result<store::Statement> clear = db.prepare("DELETE FROM logon_impact WHERE logon = ?1");
    if (!clear) {
      return tl::unexpected(clear.error());
    }
    if (Result<void> cleared = clear->bind(1, seconds).run(); !cleared) {
      return cleared;
    }
    Result<store::Statement> mark =
        db.prepare("INSERT OR IGNORE INTO logon_recorded(logon) VALUES (?1)");
    if (!mark) {
      return tl::unexpected(mark.error());
    }
    if (Result<void> marked = mark->bind(1, seconds).run(); !marked) {
      return marked;
    }
    Result<store::Statement> insert = db.prepare(
        "INSERT INTO logon_impact(logon, image, cpu_seconds, io_bytes) VALUES (?1, ?2, ?3, ?4)");
    if (!insert) {
      return tl::unexpected(insert.error());
    }
    for (const ImageImpact& entry : impact) {
      Result<void> inserted = insert->bind(1, seconds)
                                  .bind(2, std::string_view{entry.image})
                                  .bind(3, entry.cpu_seconds)
                                  .bind(4, static_cast<std::int64_t>(entry.io_bytes))
                                  .run();
      if (!inserted) {
        return inserted;
      }
    }
    return {};
  });
}

Result<bool> logon_recorded(store::Store& store, WallTime logon) {
  bool recorded = false;
  Result<void> read = store.read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> statement =
        db.prepare("SELECT COUNT(*) FROM logon_recorded WHERE logon = ?1");
    if (!statement) {
      return tl::unexpected(statement.error());
    }
    statement->bind(1, store::to_unix_seconds(logon));
    Result<bool> row = statement->step();
    if (!row) {
      return tl::unexpected(row.error());
    }
    recorded = *row && statement->column_int(0) > 0;
    return {};
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return recorded;
}

namespace {

// Every row of `statement`, read with `row_of`.
template <typename T, typename RowOf>
Result<std::vector<T>> all_rows(store::Statement& statement, RowOf row_of) {
  std::vector<T> rows;
  for (;;) {
    Result<bool> row = statement.step();
    if (!row) {
      return tl::unexpected(row.error());
    }
    if (!*row) {
      return rows;
    }
    rows.push_back(row_of(statement));
  }
}

}  // namespace

Result<std::vector<std::vector<ImageImpact>>> logon_history(store::Store& store,
                                                            std::size_t count) {
  std::vector<std::vector<ImageImpact>> history;
  Result<void> read = store.read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> logons =
        db.prepare("SELECT logon FROM logon_recorded ORDER BY logon DESC LIMIT ?1");
    if (!logons) {
      return tl::unexpected(logons.error());
    }
    logons->bind(1, static_cast<std::int64_t>(count));
    Result<std::vector<std::int64_t>> times = all_rows<std::int64_t>(
        *logons, [](const store::Statement& row) { return row.column_int(0); });
    if (!times) {
      return tl::unexpected(times.error());
    }
    for (const std::int64_t time : *times) {
      Result<store::Statement> rows = db.prepare(
          "SELECT image, cpu_seconds, io_bytes FROM logon_impact WHERE logon = ?1 ORDER BY image");
      if (!rows) {
        return tl::unexpected(rows.error());
      }
      rows->bind(1, time);
      Result<std::vector<ImageImpact>> logon =
          all_rows<ImageImpact>(*rows, [](const store::Statement& row) {
            return ImageImpact{.image = row.column_text(0),
                               .cpu_seconds = row.column_double(1),
                               .io_bytes = static_cast<std::uint64_t>(row.column_int(2))};
          });
      if (!logon) {
        return tl::unexpected(logon.error());
      }
      history.push_back(*std::move(logon));
    }
    return {};
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return history;
}

Result<void> LogonRecorder::tick(WallTime now, std::optional<WallTime> logon,
                                 std::span<const Usage> sample, const StartSource& starts) {
  if (!logon) {
    return {};
  }
  if (logon != logon_) {
    logon_ = logon;
    end_.reset();
    baseline_.clear();
    Result<bool> recorded = logon_recorded(*store_, *logon);
    if (!recorded) {
      done_ = true;
      return tl::unexpected(recorded.error());
    }
    done_ = *recorded || now - *logon > baseline_grace;
    if (!done_) {
      baseline_.assign(sample.begin(), sample.end());
    }
    return {};
  }
  if (done_) {
    return {};
  }
  const WallTime window_end = *logon + logon_window;
  if (!end_ && now >= window_end) {
    end_.emplace(sample.begin(), sample.end());
  }
  if (!end_ || now < window_end + flush_delay) {
    return {};
  }
  done_ = true;
  Result<std::vector<Start>> started = starts(*logon, window_end);
  if (!started) {
    return tl::unexpected(started.error());
  }
  return record_logon(*store_, *logon, logon_impact(*logon, *started, baseline_, *end_));
}

}  // namespace myclean::startup
