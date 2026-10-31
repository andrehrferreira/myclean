#include "core/alerts/schema.hpp"

#include "core/store/migrations.hpp"

namespace myclean::alerts {

const store::MigrationSet& alert_migrations() {
  static const store::MigrationSet set{.component = "alerts",
                                       .migrations = {
                                           {.version = 1, .name = "alert", .sql = R"sql(
CREATE TABLE alert(
  id INTEGER PRIMARY KEY,
  rule TEXT NOT NULL,
  entity TEXT NOT NULL,
  title TEXT NOT NULL,
  severity TEXT NOT NULL,
  value REAL NOT NULL,
  limit_value REAL,
  previous_value REAL,
  unit TEXT NOT NULL DEFAULT '',
  detail TEXT NOT NULL DEFAULT '',
  first_at INTEGER NOT NULL,
  last_at INTEGER NOT NULL,
  count INTEGER NOT NULL,
  active INTEGER NOT NULL);
CREATE INDEX alert_last ON alert(last_at);
CREATE TABLE alert_snooze(
  rule TEXT NOT NULL,
  entity TEXT NOT NULL,
  until INTEGER NOT NULL,
  PRIMARY KEY(rule, entity));
CREATE TABLE alert_baseline(
  rule TEXT NOT NULL,
  entity TEXT NOT NULL,
  value REAL NOT NULL,
  PRIMARY KEY(rule, entity));
)sql"},
                                       }};
  return set;
}

}  // namespace myclean::alerts
