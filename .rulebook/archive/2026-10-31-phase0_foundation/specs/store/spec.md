# Store Specification

## ADDED Requirements

### Requirement: Database Location and Mode
The store MUST keep its state in a single SQLite database at `%ProgramData%\MyClean\myclean.db`, opened in WAL journal mode, with an ACL that grants write access only to SYSTEM and Administrators and read access to the owner.

#### Scenario: Database opened in WAL mode
Given no database exists
When the store opens for the first time
Then `%ProgramData%\MyClean\myclean.db` is created
And `PRAGMA journal_mode` returns `wal`

#### Scenario: Owner cannot write the database
Given an existing database created by the service
When a process running with the owner's non-elevated token opens it for writing
Then access is denied, while opening it read-only succeeds

### Requirement: Integrity Check at Open
The store MUST run an integrity check every time it opens the database and MUST refuse to start, with an error naming the database and the failed check, when the check does not pass.

#### Scenario: Corrupt database refused
Given a database file whose pages were corrupted
When the store opens it
Then the integrity check fails
And the store reports the failure and performs no reads or writes on that file

#### Scenario: Healthy database opens
Given a database that passes `PRAGMA integrity_check`
When the store opens it
Then the store becomes available for reads and writes

### Requirement: Versioned Migrations
The store MUST track a schema version and apply every pending migration in order, each in its own transaction, so that a failed migration leaves the schema at the last successful version.

#### Scenario: Fresh database migrated to latest
Given an empty database
When the store opens
Then every migration is applied in order and the schema version equals the latest migration number

#### Scenario: Failed migration rolls back
Given a database at version 3 and a migration 4 that fails halfway
When the store opens
Then migration 4 is rolled back, the schema version stays 3 and the store reports the error

### Requirement: Additive-Only Migrations
Every migration MUST be additive (new tables, new nullable or defaulted columns, new indexes) and SHALL NOT drop, rename or change the type of an existing table or column, so that an older build can open a database migrated by a newer build. An older build opening a database whose schema version is higher than its own SHALL open it without migrating, ignore tables and columns it does not know, and keep working.

#### Scenario: Rollback after a migration keeps history
Given build B2 migrated the database from schema version 5 to 6
When deploy.ps1 rolls back to build B1, which knows schema version 5
Then B1 opens the database, leaves the schema version at 6, ignores the version-6 additions and keeps all collected history

#### Scenario: Destructive migration rejected
Given a new migration that drops or renames a column
When the migration test suite runs
Then the suite fails and names the offending migration

### Requirement: Batched Writes
The store MUST buffer telemetry, event and inventory writes and flush them in one transaction at most every 10 s, and on a clean shutdown MUST flush every buffered write before closing.

#### Scenario: Writes flushed every 10 s
Given 500 events queued over 4 s
When 10 s have passed since the previous flush
Then all 500 events are committed in a single transaction

#### Scenario: Clean shutdown flushes the buffer
Given 20 buffered events not yet flushed
When the store is closed cleanly
Then all 20 events are committed before the database is closed

### Requirement: Synchronous Audit Writes
Writes to `action_log`, `undo_blob` and `quarantine` MUST be committed synchronously, before the caller continues, and SHALL NOT pass through the write batch.

#### Scenario: Audit record survives a crash
Given an action whose `applying` transition has just been recorded
When the process is terminated immediately afterwards
Then the `applying` record is present in `action_log` after restart
And buffered telemetry from the last 10 s may be missing

### Requirement: Time-Series Tiers
The time-series storage MUST keep 1 s samples in an in-memory ring holding the last 1 h, minute aggregates in `ts_minute` and hour aggregates in `ts_hour`, keyed by metric, entity and time, with each aggregate carrying at least the minimum, maximum and mean of its samples.

#### Scenario: Minute aggregate from raw samples
Given 60 one-second CPU samples for core 3 within one minute with values 10 to 69
When the minute closes
Then one `ts_minute` row exists for (cpu.core, 3, that minute) with min 10, max 69 and mean 39.5

#### Scenario: Hour aggregate from minute aggregates
Given 60 minute rows for one metric and entity within one hour
When the hour closes
Then one `ts_hour` row exists whose min, max and mean are consistent with the 60 minute rows

#### Scenario: Ring holds only the last hour
Given the sampler has produced 1 s samples for 90 min
When raw samples are queried
Then only samples from the last 60 min are returned

### Requirement: Retention
The store MUST delete minute aggregates older than 30 days, hour aggregates and event rows (`proc_start`, `governor_event`, `input_incident`, `alert`, `health_sample`) older than 2 years, and findings and plans older than 2 years, and MUST never delete rows from `action_log`, `undo_blob`, `schedule`, `snapshot` or `report` through retention.

#### Scenario: Old minute rows removed
Given `ts_minute` rows 31 days old and 29 days old
When the retention job runs
Then the 31-day rows are deleted and the 29-day rows remain

#### Scenario: Audit log untouched by retention
Given `action_log` rows older than 2 years
When the retention job runs
Then every `action_log` row remains

### Requirement: Schema-Validated Data Loading
The loaders for `data/rules/*.toml`, `data/classes.toml` and `data/kb/*.json` MUST validate each file against its schema and MUST reject an invalid file whole, keeping the last valid version of that file in effect; an invalid file SHALL never be partially applied.

#### Scenario: Invalid rule file rejected whole
Given a rule file with ten valid rules and one rule missing its required `tier` field
When the rules are loaded
Then none of the eleven rules from that file are loaded
And the error names the file, the rule and the missing field
And rules from other valid files are loaded

#### Scenario: Invalid reload keeps previous version
Given `data/classes.toml` was loaded successfully
When it is edited to contain a malformed regular expression and reloaded
Then the reload is rejected and the previously loaded classes stay in effect

#### Scenario: Valid knowledge-base file loaded
Given a `data/kb/processes.json` file that matches its schema
When the knowledge base is loaded
Then every entry in the file is available to lookups
