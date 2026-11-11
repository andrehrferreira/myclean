# P0 gaps and fixes

This page lists every row of [01-p0-map.md](01-p0-map.md) that is not **met**: 14 partial and 4 no test. The audit covers HEAD `dfe60cc`.

Each item gives what is missing, the fix, the files it touches and a size: S is under half a day, M is one or two days, L is more. Items are in priority order.

## Partial

### UI-3: 9 of 13 screens are missing (L overall)

Missing screens: Placement, Optimize, Startup, Apps, Security, Backups, Reports, Audit, Settings. The services behind most of them already exist, so each screen is a tray view over existing methods.

| Screen | What it needs | Size |
|---|---|---|
| Startup | A view over `startup.list` (impact) and `startup.boot_times`, with disable and enable buttons that call `plans.propose_startup` | M |
| Apps | A view over `apps.list`, with uninstall through `plans.propose_uninstall` | M |
| Security | A view over `security.scan`, `security.extensions` and `security.network` (read only) | M |
| Backups | A view over `backups.check` (read only) | S |
| Placement | Relocation proposals and the system-space view from PLC-2 | M |
| Optimize | The index exclusion, VHDX compaction, Dev Drive and verification results already exposed over IPC | M |
| Audit | A new read method `audit.list` over `action_log` (paged, newest first), plus the screen | M |
| Settings | Nothing exists today: governor mode, VirusTotal key status, workspace roots. Start with read only | M |
| Reports | Wait for phase6 1.1 and 1.2, then show the report list with an "Open" button | S |

Each screen needs a parser in `core/tray_model`, a `Section` entry and a draw function in `tray/`, and a case in `DashboardUiTest.DrawsEverySectionWithLiveData`.

**Files:** `src/core/tray_model/{poller,replies,dashboard_model}.*`, new `src/tray/<screen>_ui.cpp`, `src/tray/dashboard_ui.cpp`, `tests/unit/tray_model/*`, `tests/unit/tray/dashboard_ui_test.cpp`.

### UI-5: the CLI misses many read features (M)

No CLI command calls these read methods:
- `space.top`, `space.old_downloads`, `space.duplicates`, `space.used_installers`, `space.owner_decisions`, `space.dev_drive`, `space.vhdx`, `space.indexed_folders`
- `startup.list`, `startup.boot_times`
- `plans.list`, `plans.get`, `quarantine.list`
- `resident.costs`, `resident.trend`
- `input.pauses`, `governor.events`, `cleaning.status`

**Fix:** most of these take no parameters, so they fit the `plain_commands` table. Add `space <list>`, `startup`, `boot-times`, `plans [<id>]`, `quarantine`, `costs` and `trend <pid>`. Extend `CliTest.EveryReadCommandAnswersInBothForms` to cover them, and to cover `apps`, `backups` and `security`, which have no CLI test today.

**Files:** `src/cli/cli.cpp`, `src/cli/text.{hpp,cpp}`, `tests/unit/cli/cli_test.cpp`.

### OPT-1: no client can enable a startup item (S + M + admin S)

1. **(S)** Give MCP a way to enable: either an `enable` argument on `propose_disable_startup` or a new `propose_enable_startup` tool. Files: `src/core/mcp/server.cpp`, `tests/unit/mcp/server_test.cpp`.
2. **(M)** Add the tray Startup screen (see UI-3).
3. **(S, admin)** Run phase4 1.1 (spike X6): the HKLM, task and service round trip. Record it in `docs/spikes/X6-startup.md`.

### PLC-2: automatic changes contradict the PRD, and there is no cost view (S + M)

1. **(S)** Write the 2026-11-01 owner decision as `.rulebook/decisions/003-automatic-system-space-on-critical-c.md`.
   - Amend PRD PLC-2 to say "changed only on approval, except below 10 % free on C:, where hibernation and the page file change automatically".
   - Fix the comments that still say approval only: `src/core/placement/system_space.hpp:19-20` and `src/core/ipc/plans.hpp:140`.
2. **(M)** Add a read method `system_space.status` that returns the three `SettingSummary` values: current state, cost and recommendation. Show it in the Placement screen with propose buttons.
   - Files: `src/core/cleaning/service.cpp`, `src/core/ipc/plans.hpp`, `src/core/mcp/server.cpp`, tray, tests.
   - Fix the tray text "see System space" in `src/core/tray_model/cleaning.cpp:462`, which points to a view that does not exist.

### SPC-1: no treemap, and no top-N of any folder (M + L)

1. **(M)** Let `space.top` accept an absolute `path` that passes `safety::normalize_path`, and include the path in the candidate key. Add the same argument to MCP `storage_top`.
   - Files: `src/core/cleaning/service.cpp` (`analyze_top`), `src/core/ipc/plans.hpp`, `src/core/mcp/server.cpp`, `tests/unit/cleaning/service_test.cpp`, `tests/unit/mcp/server_test.cpp`.
2. **(L)** Add a squarified treemap layout over `ScanResult`, with tests on the rectangle math, and draw it with ImGui draw lists in the Space section.
   - Files: new `src/core/space/treemap.{hpp,cpp}`, `tests/unit/space/treemap_test.cpp`, `src/tray/inbox_ui.cpp`.

### SPC-5: "large forgotten files" is missing (M)

**Fix:** add `space::forgotten_files(scan, min_size, idle_days)` over the existing `ScanResult`. It uses the newer of the modified and created times, and skips cloud placeholders.
- Expose it as `space.forgotten_files` and MCP `find_forgotten_files`.
- Show it in the Space section.

**Files:** `src/core/space/forgotten.{hpp,cpp}`, `src/core/cleaning/service.cpp`, `src/core/ipc/plans.hpp`, `src/core/mcp/server.cpp`, `src/tray/inbox_ui.cpp`, `tests/unit/space/forgotten_test.cpp`.

### GOV-6: the owner's game list cannot be reached (M)

**Fix:** add `[games] owner = [...]` and `roots = [...]` to `data/governor.toml`, or a store table edited from the Governor screen. Pass them into `GameSources` in `read_games`; today it passes `{}`.

**Files:** `src/svc/main.cpp:221-236`, `src/core/governor/policy.{hpp,cpp}`, `src/tray/governor_ui.cpp`, `tests/unit/governing/games_test.cpp`, `tests/unit/governor/policy_test.cpp`.

### GOV-4: the ladder differs from the PRD text (S)

These deviations are owner-approved:
- Priority and memory priority are one step.
- Cap and suspend apply only to build/test trees by default.
- Suspend needs a memory signal.
- Build/test trees are chosen before heavier trees.

**Fix:** amend PRD §6.6 GOV-4 to match and cite commit 0fd045c. Splitting memory priority into its own step would be M (`src/core/governor/commands.hpp`, `ladder.cpp`, `src/core/governing/executor.cpp`, tests), but nothing asks for it.

### MON-6: the signer can be missing, and stops are not stored (M + S)

1. **(M)** The signer is null on the first start of an image the trust cache has not seen. When the trust worker resolves an image, run `UPDATE proc_start SET signer=? WHERE image=? AND signer IS NULL`. Also warm the cache from the seed snapshot at startup.
   - Files: `src/svc/trust_component.{hpp,cpp}`, `src/core/events/event_store.{hpp,cpp}`, `src/svc/main.cpp`, `tests/unit/events/event_store_test.cpp`, `tests/unit/svc/trust_component_test.cpp`.
2. **(S)** Persist stops in a `proc_stop` table (additive migration) from `EventRecorder::on_stop`.
   - Files: `src/core/events/event_store.cpp`, `tests/unit/events/event_store_test.cpp`.

### NFR-SAFE-1: deletes outside SafeRemove (S + M)

1. **(S)** List the deliberate exceptions in architecture §6 and in PRD NFR-SAFE-1:
   - the Recycle Bin purge through the shell;
   - cleanup handlers and DISM;
   - WSL commands (`rm -rf /tmp/...`, `wsl --unregister`);
   - MyClean's own staging and `.npmrc` files;
   - pruning of MyClean's own crash dumps.
2. **(M)** Route MyClean's own-file deletes through a small guarded helper that applies `deny_list` and refuses reparse points.
   - Files: `src/platform/service/crash_dump.cpp:87`, `src/platform/settings/user_settings.cpp:147-161`.

### NFR-PERF-2: the open time is not measured (S + M)

1. **(S)** Measure and log the time from the tray click to the first presented frame in `src/tray/dashboard_window.cpp`.
2. **(M)** Add a fake-clock end-to-end test: TelemetryComponent, then Poller, then DashboardModel over 600 s. It asserts that the chart equals `Store::raw`.
   - Files: `tests/unit/tray_model/` (new test).
3. Related: cache the fill projection (see risk 1 below). It removes about 10k row reads per volume per second from the 1 s path.

### NFR-REL-1: failure actions are set only once (S)

**Fix:** re-apply `sc failure` and `sc failureflag` on every deploy, not only on first registration. Read them back with `sc qfailure` as part of the deploy health check.

**Files:** `tools/deploy/deploy.ps1:296-310`.

### NFR-REL-2: restart is not within 5 s, and priority survives a crash (S + M)

1. **(S)** Lower the SCM restart delay to 1000-2000 ms in `deploy.ps1:303`. This and the NFR-REL-1 fix are one change.
2. **(M)** Persist the saved priority, memory priority and EcoQoS values the way suspends are persisted, so that `recover` restores them after a crash.
   - Files: `src/core/governing/executor.cpp:137-153`, `src/core/governing/records.*`, `src/core/governor/recovery.cpp`, `tests/unit/governing/recovery_test.cpp`.
3. **(M, manual)** Add a LocalSystem acceptance step: kill the service under a capped build and check re-adoption.

### NFR-QA-1: CI is red on main (S)

Two live tests fail on the GitHub runner:
- `Win32DiagnosticsLogTest.ReadsEvents100And101OrReportsThatItMayNot` (`tests/unit/platform_diagnostics/win32_diagnostics_log_test.cpp`)
- `Win32SeekPenalty.AtLeastOneDiskAnswers` (`tests/unit/platform_settings/`)

**Fix:** make both accept "not available on this machine", as the other live tests do, or skip them when there is no physical disk or event log access. Push only once CI is green. Commits go straight to main during the pilot, so "green before merge" in the PRD means "green on main".

## No test

### NFR-PERF-1: the budget is not enforced (S + M)

1. **(S)** Assert a CPU and private-bytes budget for the collectors in `tests/unit/platform_perf/pipeline_smoke_test.cpp`.
2. **(M)** Add an alert rule on `self.cpu` and `self.memory` averaged over 1 h, against the 1 % and 150 MB limits. Phase 3 recorded an 873 MB service peak.
   - Files: `data/alerts.toml`, `src/svc/telemetry_component.cpp`, `tests/unit/svc/alerts_component_test.cpp`.

### NFR-OPS-2: the automatic rollback is never exercised (M)

**Fix:** add a Pester test, or a `-SelfTest` mode of `deploy.ps1` that runs against a temporary install root. Stage a build whose `status --json` reports unhealthy, then assert that the previous version is current again. Run it in CI.

**Files:** `tools/deploy/deploy.ps1`, new `tools/deploy/deploy.Tests.ps1`, `.github/workflows/ci.yml`.

### NFR-SEC-2 and NFR-OPS-1: nothing guards against a regression (S)

**Fix:** add a CMake script check next to `check-core-includes`. It fails on:
- `schtasks`, `Register-ScheduledTask`, `RegisterTaskDefinition`, `NewTask(`;
- `CreateService` with `SERVICE_KERNEL_DRIVER`;
- any `.sys` file in the install tree.

**Files:** new `cmake/scripts/check-forbidden.cmake`, `cmake/Lint.cmake`.

## Correctness risks found along the way

These are not P0 gaps, but they can make a "met" row wrong in practice. File:line refers to HEAD.

1. **A heavy query every second.** `metrics.latest` computes the days-until-full projection for every volume on each call, reading about 7 days of minute rows (svc/telemetry_component.cpp:293, 562-575). The tray calls it every second even when the dashboard is closed. Cache it per 60 s volume sample.
2. **Report-view "send to Recycle Bin" is a dead end.**
   - `Executor::recycle` (cleaning/executor.cpp:1073) stores an approved plan of kind `cleanup`, and `user_tasks::parse_kind` refuses that kind (user_tasks/task.cpp:80-99). The tray also never calls `plans.recycle`.
   - The task fails every time `user_tasks.next` hands it out. The only test stops at `user_tasks.next`.
3. **Children can escape a build's job.** Children the root spawned before its ETW start was handled are not placed in the job: only the root is assigned (governing/governor.cpp:427-437, 480). Events queue behind `tick()` on one thread, which widens the window.
4. **No rollback while telemetry stalls.** If the process view stops advancing or a CPU or commit metric is missing, `govern()` does not run (governor.cpp:543-548, 598-600). Steps 1-3 then stay applied until a stop.
5. **Priority and EcoQoS survive a crash.** They are saved only in memory (governing/executor.cpp:137-153). The below-normal priority set when a tree is placed in a job (governor.cpp:495) is never restored, even on a clean stop.
6. **A suspended tree can lose its Resume button.** If the root exits while children stay suspended, the notification is erased (governor.cpp:549-552). The same happens when the list passes 32 entries (governor.cpp:358-360). The owner then has no Resume until a pause or stop.
7. **A failed owner Resume leaves a hidden suspend.** The ladder marks the processes resumed even when the OS resume failed (ladder.cpp:420-424). The executor keeps the record (executor.cpp:316-317), so the process stays suspended with no tray action.
8. **Pipe methods open to any owner process.** `cleaning.scan` (runs the auto tier at once), `alerts.snooze` and `alerts.dismiss` are `Access::any`. An owner-run AI client that talks to the pipe directly, not through MCP, can trigger auto deletions early or silence alerts. Consider making the alert methods tray-only.
9. **Dev artifacts are deleted permanently.** The node `artifacts` rule is tier auto, so `dist`, `build`, `coverage` and `node_modules` are deleted, not quarantined (executor.cpp:313). The guards are git-ignored, untracked, 30 days inactive and not in use. An ignored release build cannot be undone.
10. **Quarantine can land on a full volume.** `quarantine_root` returns empty when `drives->volumes()` fails, so items quarantine on their own volume even when it is critically low (executor.cpp:579-581).
11. **A rule that can never act.** `windows/live-kernel-reports` (data/packs/windows/clean.toml:53) targets `%WINDIR%\LiveKernelReports`, which the deny list does not allow (safety/deny_list.cpp:18).
12. **Disk temperature history cannot be read back.** `metrics.query` rejects `disk.temperature` (telemetry_component.cpp:317-337). The series is also keyed by disk number, which can change, while alerts use model plus serial.
13. **The collector list is duplicated.** `svc::add_collectors` (telemetry_component.cpp:169-182) repeats `telemetry::register_collectors`, and only the latter is tested.
14. **The governor pause is held in memory.** A service restart ends it without telling the owner.
