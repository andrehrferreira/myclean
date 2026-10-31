#include "core/actions/schema.hpp"

#include "core/store/migrations.hpp"

namespace myclean::actions {

const store::MigrationSet& action_migrations() {
  static const store::MigrationSet set{.component = "actions",
                                       .migrations = {
                                           {.version = 1, .name = "action_log", .sql = R"sql(
CREATE TABLE action_log(
  id INTEGER PRIMARY KEY,
  time INTEGER NOT NULL,
  plan_id INTEGER NOT NULL,
  item_id INTEGER,
  actor TEXT NOT NULL,
  event TEXT NOT NULL,
  from_state TEXT,
  to_state TEXT,
  outcome TEXT NOT NULL,
  reason TEXT NOT NULL DEFAULT '',
  detail TEXT NOT NULL DEFAULT '',
  metrics TEXT NOT NULL DEFAULT '');
CREATE INDEX action_log_plan ON action_log(plan_id, id);
CREATE TRIGGER action_log_no_update BEFORE UPDATE ON action_log
BEGIN
  SELECT RAISE(ABORT, 'action_log is append-only');
END;
CREATE TRIGGER action_log_no_delete BEFORE DELETE ON action_log
BEGIN
  SELECT RAISE(ABORT, 'action_log is append-only');
END;
CREATE TABLE undo_blob(
  id INTEGER PRIMARY KEY,
  log_id INTEGER NOT NULL REFERENCES action_log(id),
  plan_id INTEGER NOT NULL,
  item_id INTEGER,
  data BLOB NOT NULL,
  not_reversible TEXT NOT NULL DEFAULT '');
CREATE INDEX undo_blob_plan ON undo_blob(plan_id);
CREATE TABLE quarantine(
  id INTEGER PRIMARY KEY,
  run_id INTEGER NOT NULL,
  item_id INTEGER NOT NULL,
  original_path TEXT NOT NULL,
  quarantine_path TEXT NOT NULL,
  volume_serial INTEGER NOT NULL,
  file_id BLOB NOT NULL,
  size INTEGER NOT NULL,
  moved_at INTEGER NOT NULL,
  purge_after INTEGER NOT NULL,
  state TEXT NOT NULL);
CREATE INDEX quarantine_run ON quarantine(run_id);
CREATE INDEX quarantine_state ON quarantine(state, purge_after);
)sql"},
                                           {.version = 2, .name = "plan_params", .sql = R"sql(
CREATE TABLE plan_params(
  plan_id INTEGER PRIMARY KEY,
  params TEXT NOT NULL);
)sql"},
                                       }};
  return set;
}

}  // namespace myclean::actions
