#include "core/store/schema.hpp"

#include "core/store/migrations.hpp"

namespace myclean::store {

const MigrationSet& store_migrations() {
  static const MigrationSet set{.component = "store",
                                .migrations = {
                                    {.version = 1, .name = "time_series", .sql = R"sql(
CREATE TABLE series(
  id INTEGER PRIMARY KEY,
  metric TEXT NOT NULL,
  entity TEXT NOT NULL,
  UNIQUE(metric, entity));
CREATE TABLE ts_minute(
  series INTEGER NOT NULL REFERENCES series(id),
  time INTEGER NOT NULL,
  min REAL NOT NULL,
  max REAL NOT NULL,
  mean REAL NOT NULL,
  count INTEGER NOT NULL,
  PRIMARY KEY(series, time)) WITHOUT ROWID;
CREATE INDEX ts_minute_time ON ts_minute(time);
CREATE TABLE ts_hour(
  series INTEGER NOT NULL REFERENCES series(id),
  time INTEGER NOT NULL,
  min REAL NOT NULL,
  max REAL NOT NULL,
  mean REAL NOT NULL,
  count INTEGER NOT NULL,
  PRIMARY KEY(series, time)) WITHOUT ROWID;
CREATE INDEX ts_hour_time ON ts_hour(time);
)sql"},
                                    {.version = 2, .name = "findings_and_plans", .sql = R"sql(
CREATE TABLE finding(
  id INTEGER PRIMARY KEY,
  kind TEXT NOT NULL,
  subject TEXT NOT NULL,
  severity TEXT NOT NULL,
  summary TEXT NOT NULL,
  detected_at INTEGER NOT NULL);
CREATE INDEX finding_detected_at ON finding(detected_at);
CREATE TABLE plan(
  id INTEGER PRIMARY KEY,
  action TEXT NOT NULL,
  finding_id INTEGER REFERENCES finding(id) ON DELETE SET NULL,
  proposer TEXT NOT NULL,
  state TEXT NOT NULL,
  created_at INTEGER NOT NULL);
CREATE INDEX plan_created_at ON plan(created_at);
CREATE INDEX plan_finding ON plan(finding_id);
CREATE TABLE plan_item(
  id INTEGER PRIMARY KEY,
  plan_id INTEGER NOT NULL REFERENCES plan(id) ON DELETE CASCADE,
  path TEXT NOT NULL,
  volume_serial INTEGER NOT NULL,
  file_id BLOB NOT NULL,
  size INTEGER NOT NULL,
  approval TEXT NOT NULL);
CREATE INDEX plan_item_plan ON plan_item(plan_id);
)sql"},
                                    {.version = 3, .name = "schedule", .sql = R"sql(
CREATE TABLE schedule(
  job TEXT PRIMARY KEY,
  expression TEXT NOT NULL,
  last_run INTEGER,
  last_result TEXT,
  next_run INTEGER NOT NULL);
CREATE TABLE schedule_run(
  id INTEGER PRIMARY KEY,
  job TEXT NOT NULL,
  started_at INTEGER NOT NULL,
  catch_up INTEGER NOT NULL,
  status TEXT NOT NULL,
  detail TEXT NOT NULL DEFAULT '');
CREATE INDEX schedule_run_job ON schedule_run(job, started_at);
)sql"},
                                }};
  return set;
}

}  // namespace myclean::store
