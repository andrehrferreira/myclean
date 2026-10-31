#include "core/alerts/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/alerts/config.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/store/database.hpp"
#include "core/store/store.hpp"

namespace myclean::alerts {

namespace {

constexpr std::string_view alert_columns =
    "id, rule, entity, title, severity, value, limit_value, previous_value, unit, detail, "
    "first_at, last_at, count, active";

std::optional<double> optional_double(const store::Statement& row, int column) {
  return row.is_null(column) ? std::nullopt : std::optional(row.column_double(column));
}

Alert read_alert(const store::Statement& row) {
  return Alert{.id = AlertId{row.column_int(0)},
               .key = {.rule = row.column_text(1), .entity = row.column_text(2)},
               .title = row.column_text(3),
               .severity = parse_severity(row.column_text(4)).value_or(Severity::warning),
               .value = row.column_double(5),
               .limit = optional_double(row, 6),
               .previous = optional_double(row, 7),
               .unit = row.column_text(8),
               .detail = row.column_text(9),
               .first = store::from_unix_seconds(row.column_int(10)),
               .last = store::from_unix_seconds(row.column_int(11)),
               .count = row.column_int(12),
               .active = row.column_int(13) != 0};
}

// Runs `query` with `bind` applied and passes every row to `each`.
template <typename Bind, typename Each>
Result<void> for_each_row(store::Database& db, const std::string& query, Bind bind, Each each) {
  Result<store::Statement> statement = db.prepare(query);
  if (!statement) {
    return tl::unexpected(std::move(statement.error()));
  }
  bind(*statement);
  Result<bool> row = statement->step();
  for (; row && *row; row = statement->step()) {
    each(*statement);
  }
  if (!row) {
    return tl::unexpected(std::move(row.error()));
  }
  return {};
}

void bind_optional(store::Statement& statement, int index, std::optional<double> value) {
  if (value) {
    statement.bind(index, *value);
  } else {
    statement.bind(index, nullptr);
  }
}

}  // namespace

AlertEngine::AlertEngine(store::Store& store, const os::IClock& clock, AlertConfig config)
    : store_(&store), clock_(&clock), config_(std::move(config)) {}

Result<std::unique_ptr<AlertEngine>> AlertEngine::open(store::Store& store, const os::IClock& clock,
                                                       AlertConfig config) {
  if (Result<void> added = store.add_retention(
          {.table = "alert", .time_column = "last_at", .max_age = store::Store::history_retention});
      !added) {
    return tl::unexpected(std::move(added.error()));
  }
  // The constructor is private, so std::make_unique cannot call it.
  std::unique_ptr<AlertEngine> engine(new AlertEngine(store, clock, std::move(config)));
  if (Result<void> loaded = engine->load(); !loaded) {
    return tl::unexpected(std::move(loaded.error()));
  }
  return engine;
}

Result<void> AlertEngine::load() {
  const WallTime now = this->now();
  const std::int64_t window_start = store::to_unix_seconds(now - config_.rate_limit);
  return store_->write([&](store::Database& db) -> Result<void> {
    // The latest record of each key that can still take occurrences.
    Result<void> alerts = for_each_row(
        db,
        "SELECT " + std::string(alert_columns) +
            " FROM alert WHERE active = 1 OR last_at >= ?1 ORDER BY id",
        [window_start](store::Statement& query) { query.bind(1, window_start); },
        [this](const store::Statement& row) {
          Alert alert = read_alert(row);
          State& state = states_[alert.key];
          // An active threshold alert means the latest reading was beyond its limit.
          state.beyond = alert.active && alert.limit.has_value();
          state.alert = std::move(alert);
        });
    if (!alerts) {
      return alerts;
    }
    Result<void> baselines = for_each_row(
        db, "SELECT rule, entity, value FROM alert_baseline", [](store::Statement&) {},
        [this](const store::Statement& row) {
          states_[AlertKey{.rule = row.column_text(0), .entity = row.column_text(1)}].baseline =
              row.column_double(2);
        });
    if (!baselines) {
      return baselines;
    }
    Result<store::Statement> expired = db.prepare("DELETE FROM alert_snooze WHERE until <= ?1");
    if (!expired) {
      return tl::unexpected(std::move(expired.error()));
    }
    if (Result<void> deleted = expired->bind(1, store::to_unix_seconds(now)).run(); !deleted) {
      return deleted;
    }
    return for_each_row(
        db, "SELECT rule, entity, until FROM alert_snooze", [](store::Statement&) {},
        [this](const store::Statement& row) {
          snoozes_.insert_or_assign(
              AlertKey{.rule = row.column_text(0), .entity = row.column_text(1)},
              store::from_unix_seconds(row.column_int(2)));
        });
  });
}

void AlertEngine::set_config(AlertConfig config) {
  const std::scoped_lock lock(mutex_);
  config_ = std::move(config);
}

WallTime AlertEngine::now() const {
  // Stored times are whole seconds; keeping the same precision in memory makes a reloaded
  // engine behave like the one that wrote the records.
  return std::chrono::floor<std::chrono::seconds>(clock_->now());
}

bool AlertEngine::snoozed(const AlertKey& key, WallTime now) const {
  const auto snooze = snoozes_.find(key);
  return snooze != snoozes_.end() && now < snooze->second;
}

Result<std::vector<Alert>> AlertEngine::report(std::span<const Reading> readings) {
  const std::scoped_lock lock(mutex_);
  const WallTime now = this->now();
  std::vector<Alert> toasts;
  for (const Reading& reading : readings) {
    if (std::isnan(reading.value)) {
      continue;
    }
    for (const AlertRule& rule : config_.rules) {
      if (rule.metric != reading.metric) {
        continue;
      }
      if (Result<void> evaluated = evaluate(rule, reading, now, toasts); !evaluated) {
        return tl::unexpected(std::move(evaluated.error()));
      }
    }
  }
  return toasts;
}

Result<void> AlertEngine::evaluate(const AlertRule& rule, const Reading& reading, WallTime now,
                                   std::vector<Alert>& toasts) {
  const AlertKey key{.rule = rule.id, .entity = reading.entity};
  State& state = states_[key];
  if (rule.change) {
    const std::optional<double> before = state.baseline;
    if (before == reading.value) {
      return {};
    }
    Result<void> saved = store_->write([&](store::Database& db) -> Result<void> {
      Result<store::Statement> upsert = db.prepare(
          "INSERT INTO alert_baseline(rule, entity, value) VALUES(?1, ?2, ?3) "
          "ON CONFLICT(rule, entity) DO UPDATE SET value = excluded.value");
      if (!upsert) {
        return tl::unexpected(std::move(upsert.error()));
      }
      return upsert->bind(1, key.rule).bind(2, key.entity).bind(3, reading.value).run();
    });
    if (!saved) {
      return saved;
    }
    state.baseline = reading.value;
    if (before && (*rule.change == Change::any || reading.value > *before)) {
      return fire(rule, reading, before, now, toasts);
    }
    return {};
  }

  const bool beyond = rule.below ? reading.value < *rule.below : reading.value > *rule.above;
  if (beyond && !state.beyond) {
    if (Result<void> fired = fire(rule, reading, std::nullopt, now, toasts); !fired) {
      return fired;
    }
  } else if (!beyond && state.beyond && state.alert && state.alert->active) {
    Alert ended = *state.alert;
    ended.active = false;
    if (Result<void> saved = save(std::move(ended)); !saved) {
      return saved;
    }
  }
  state.beyond = beyond;
  return {};
}

Result<void> AlertEngine::fire(const AlertRule& rule, const Reading& reading,
                               std::optional<double> previous, WallTime now,
                               std::vector<Alert>& toasts) {
  const AlertKey key{.rule = rule.id, .entity = reading.entity};
  const State& state = states_[key];
  const bool same_record =
      state.alert && (state.alert->active || now - state.alert->last < config_.rate_limit);
  Alert alert = same_record ? *state.alert : Alert{.key = key, .first = now};
  alert.title = rule.title;
  alert.severity = rule.severity;
  alert.value = reading.value;
  alert.limit = rule.below ? rule.below : rule.above;
  alert.previous = previous;
  alert.unit = rule.unit;
  alert.detail = reading.detail;
  alert.last = now;
  alert.count += 1;
  alert.active = true;
  if (Result<void> saved = save(alert); !saved) {
    return saved;
  }
  State& updated = states_[key];
  if (!snoozed(key, now) &&
      (!updated.last_toast || now - *updated.last_toast >= config_.rate_limit)) {
    updated.last_toast = now;
    toasts.push_back(*updated.alert);
  }
  return {};
}

Result<void> AlertEngine::save(Alert alert) {
  Result<void> saved = store_->write([&alert](store::Database& db) -> Result<void> {
    const bool insert = alert.id.value() == 0;
    Result<store::Statement> statement = db.prepare(
        insert ? "INSERT INTO alert(rule, entity, title, severity, value, limit_value, "
                 "previous_value, unit, detail, first_at, last_at, count, active) "
                 "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)"
               : "UPDATE alert SET rule = ?1, entity = ?2, title = ?3, severity = ?4, value = ?5, "
                 "limit_value = ?6, previous_value = ?7, unit = ?8, detail = ?9, first_at = ?10, "
                 "last_at = ?11, count = ?12, active = ?13 WHERE id = ?14");
    if (!statement) {
      return tl::unexpected(std::move(statement.error()));
    }
    statement->bind(1, alert.key.rule)
        .bind(2, alert.key.entity)
        .bind(3, alert.title)
        .bind(4, to_string(alert.severity))
        .bind(5, alert.value);
    bind_optional(*statement, 6, alert.limit);
    bind_optional(*statement, 7, alert.previous);
    statement->bind(8, alert.unit)
        .bind(9, alert.detail)
        .bind(10, store::to_unix_seconds(alert.first))
        .bind(11, store::to_unix_seconds(alert.last))
        .bind(12, alert.count)
        .bind(13, alert.active);
    if (!insert) {
      statement->bind(14, alert.id.value());
    }
    if (Result<void> ran = statement->run(); !ran) {
      return ran;
    }
    if (insert) {
      alert.id = AlertId{db.last_insert_id()};
    }
    return {};
  });
  if (saved) {
    states_[alert.key].alert = std::move(alert);
  }
  return saved;
}

Result<void> AlertEngine::snooze(const AlertKey& key, std::chrono::seconds duration) {
  if (duration <= std::chrono::seconds::zero()) {
    return fail(Errc::invalid_argument, "a snooze needs a positive duration");
  }
  const std::scoped_lock lock(mutex_);
  const WallTime until = now() + duration;
  Result<void> saved = store_->write([&](store::Database& db) -> Result<void> {
    Result<store::Statement> upsert = db.prepare(
        "INSERT INTO alert_snooze(rule, entity, until) VALUES(?1, ?2, ?3) "
        "ON CONFLICT(rule, entity) DO UPDATE SET until = excluded.until");
    if (!upsert) {
      return tl::unexpected(std::move(upsert.error()));
    }
    return upsert->bind(1, key.rule)
        .bind(2, key.entity)
        .bind(3, store::to_unix_seconds(until))
        .run();
  });
  if (saved) {
    snoozes_.insert_or_assign(key, until);
  }
  return saved;
}

Result<void> AlertEngine::dismiss(const AlertKey& key) {
  const std::scoped_lock lock(mutex_);
  const auto state = states_.find(key);
  std::optional<Alert> current = state == states_.end() ? std::nullopt : state->second.alert;
  if (!current || !current->active) {
    return fail(Errc::not_found,
                "no active alert for rule '" + key.rule + "' and '" + key.entity + "'");
  }
  current->active = false;
  return save(*std::move(current));
}

std::vector<Alert> AlertEngine::active() const {
  const std::scoped_lock lock(mutex_);
  const WallTime now = this->now();
  std::vector<Alert> alerts;
  for (const auto& [key, state] : states_) {
    if (state.alert && state.alert->active) {
      Alert& alert = alerts.emplace_back(*state.alert);
      if (snoozed(key, now)) {
        alert.snoozed_until = snoozes_.at(key);
      }
    }
  }
  return alerts;
}

Badge AlertEngine::badge() const {
  Badge badge;
  for (const Alert& alert : active()) {
    if (!alert.snoozed_until) {
      ++badge.count;
      badge.worst = std::max(badge.worst.value_or(alert.severity), alert.severity);
    }
  }
  return badge;
}

Result<std::vector<Alert>> AlertEngine::list(WallTime since) const {
  std::vector<Alert> alerts;
  Result<void> read = store_->read([&](store::Database& db) {
    return for_each_row(
        db,
        "SELECT " + std::string(alert_columns) +
            " FROM alert WHERE last_at >= ?1 ORDER BY last_at DESC, id DESC",
        [since](store::Statement& query) { query.bind(1, store::to_unix_seconds(since)); },
        [&alerts](const store::Statement& row) { alerts.push_back(read_alert(row)); });
  });
  if (!read) {
    return tl::unexpected(std::move(read.error()));
  }
  const std::scoped_lock lock(mutex_);
  const WallTime now = this->now();
  for (Alert& alert : alerts) {
    if (snoozed(alert.key, now)) {
      alert.snoozed_until = snoozes_.at(alert.key);
    }
  }
  return alerts;
}

}  // namespace myclean::alerts
