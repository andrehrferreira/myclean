# Reports Specification

## ADDED Requirements

### Requirement: Monthly Schedule In The In-App Scheduler
The system SHALL run the monthly job on the 1st of each month at 09:00 local time through MyClean's own scheduler (a cron-like rule stored in SQLite), and MUST NOT create any OS schedule (Task Scheduler, schtasks, cron or similar). When the machine or the service was off at the scheduled time, the job MUST run at the next service start (catch-up), and it MUST run at most once per month.

#### Scenario: Runs on day 1 at 09:00
Given the service is running on 2026-11-01 at 08:59
When the clock reaches 09:00
Then the monthly job starts and produces the report for 2026-11

#### Scenario: Catch-up at next boot
Given the PC was off from 2026-11-30 until 2026-12-03 at 10:00
When the service starts on 2026-12-03
Then the monthly job for 2026-12 runs once at that start

#### Scenario: No duplicate run in the same month
Given the 2026-12 report was produced at catch-up on 2026-12-03
When the service restarts on 2026-12-04
Then the monthly job does not run again for 2026-12

#### Scenario: No OS schedule is created
Given MyClean is installed and the monthly schedule is enabled
When the Windows Task Scheduler is listed
Then it contains no task created by MyClean

### Requirement: Monthly Snapshot
The system SHALL store a monthly snapshot containing startup items, browser extensions, services, listening ports, unsigned binaries (among process images, persistence targets and services seen), top folders with their sizes, per-drive disk health, the C: free-space trend, governor interventions, freeze incidents and space freed by `auto` cleaning during the month. The monthly job MUST run the health check (prompt 1), storage analysis (prompt 3) and intruder scan (prompt 5), and the health part MUST use 30-day aggregates from the history rather than a single sample.

#### Scenario: Health uses 30-day aggregates
Given a build tree averaged 40 % CPU over the last 30 days but is idle at the time of the run
When the monthly snapshot is taken
Then the health section lists that tree with its 30-day average, not with its current idle value

#### Scenario: Snapshot is complete
Given the monthly job runs
When the snapshot is stored
Then it contains every listed category, and a category that could not be collected is marked "not collected" with the reason instead of being empty

### Requirement: Diff Against The Previous Snapshot
The system SHALL compare the new snapshot with the most recent previous one and report added and removed startup items, extensions, services, listening ports and unsigned binaries, growth of the top folders, the change in disk health per drive, the C: free-space trend, and the counts of governor interventions, freeze incidents and space freed. When there is no previous snapshot, the report MUST state that it is the first report and serves as the baseline, without a diff section that implies changes.

#### Scenario: New extension and listening port appear in the diff
Given the previous snapshot had no extension `abc` and no listener on TCP 8080, and the new snapshot has both
When the diff is computed
Then the report lists extension `abc` as new and TCP port 8080 as a new listening port with its owning process

#### Scenario: First-ever report
Given no previous snapshot exists
When the first monthly report is produced
Then it states "first report, no previous snapshot to compare" and lists the current state as the baseline

#### Scenario: Gap of more than a month
Given the newest previous snapshot is from 2026-09 and none exists for 2026-10
When the 2026-11 report is produced
Then the diff is computed against the 2026-09 snapshot and the report names that month

### Requirement: Report Files
The system SHALL write each monthly report as a Markdown file and an HTML file under `%ProgramData%\MyClean\reports\YYYY-MM\`, covering health, storage, security, governor, freezes, disk health, space freed and the diff. The HTML file MUST be self-contained and load no remote resources.

#### Scenario: Both formats are written
Given the monthly job for 2026-11 completes
When the reports folder is inspected
Then `%ProgramData%\MyClean\reports\2026-11\` holds the Markdown and the HTML report with the same content

#### Scenario: HTML loads nothing from the network
Given the HTML report is opened in a browser with networking disabled
When it renders
Then every section and chart is displayed

### Requirement: Toast With Open Report
The system SHALL show a toast when a monthly report is ready, with an "Open report" action that opens the HTML report. The toast MUST go through the alert engine (rate-limited, also shown on the tray badge).

#### Scenario: Open report from the toast
Given the 2026-11 report has just been written
When the owner clicks "Open report" on the toast
Then the HTML report for 2026-11 opens

### Requirement: Monthly Job Changes Nothing
The system SHALL keep the monthly job read-only: it MUST NOT delete, move, quarantine, disable, uninstall or close anything, and no data from the report MUST leave the machine. Cleanup candidates found by the storage analysis MUST appear in the report only, to be acted on through plans approved in the tray.

#### Scenario: Audit log shows no action from the report run
Given the monthly job finds 30 GB of cleanup candidates and two suspicious extensions
When the job completes
Then the `action_log` contains no action executed by the monthly job, and no network request carrying report data was made

### Requirement: Report MCP Tools
The system SHALL expose `get_report(month)` and `compare_snapshots(a, b)` as MCP read tools, with the same data available from the CLI with `--json`. A request for a month or snapshot that does not exist MUST return a clear "not found" result instead of an empty report.

#### Scenario: Get an existing report
Given the 2026-11 report exists
When `get_report` is called with month `2026-11`
Then the report content and its file paths are returned

#### Scenario: Missing month
Given no report exists for 2026-05
When `get_report` is called with month `2026-05`
Then the result says that no report exists for 2026-05

#### Scenario: Compare any two snapshots
Given snapshots exist for 2026-11 and 2027-01
When `compare_snapshots` is called with those two snapshots
Then the same diff categories as the monthly report are returned for that pair

### Requirement: Seven-Prompt Acceptance Suite
The system SHALL ship an automated acceptance suite that calls the MCP tools for prompts 1 to 7 against a fixture machine state and checks each prompt's pass condition from docs/prd.md §7, plus a manual pass of each prompt pasted as written in Claude Code and in the MyClean UI before release. For every prompt the suite MUST assert that the audit log shows no action without a tray approval.

#### Scenario: All seven prompts pass on fixtures
Given the fixture machine state used by the acceptance suite
When the suite runs
Then prompts 1 to 7 each meet their pass condition and the audit log shows no action without a tray approval

#### Scenario: Two consecutive reports produce a meaningful diff
Given two fixture snapshots one month apart where a startup item, a service and a listening port were added
When the prompt 7 test produces the second report
Then the report lists exactly those three additions

#### Scenario: Suite fails on an unapproved action
Given a fixture run in which an action appears in `action_log` without a matching tray approval
When the suite runs
Then the suite fails and names the action

### Requirement: v1.0 Release Readiness
The system SHALL be released as v1.0.0 only when every P0 requirement in docs/prd.md is mapped to its implementation and at least one test, all seven prompts pass, the full quality gate is green, and CHANGELOG.md has a v1.0.0 entry.

#### Scenario: Unmapped P0 requirement blocks release
Given the P0 audit finds a P0 requirement with no test
When v1.0.0 readiness is checked
Then the release is blocked until the gap is fixed

#### Scenario: Release with complete audit
Given every P0 requirement has an implementation and a test, all seven prompts pass and the quality gate is green
When v1.0.0 is prepared
Then CHANGELOG.md contains a v1.0.0 entry listing what v1.0 delivers
