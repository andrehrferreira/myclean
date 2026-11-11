# Development

How to build, test and extend MyClean. Design: [architecture.md](architecture.md).
Deployment on this PC: [deploy.md](deploy.md).

## Prerequisites

- Windows 11, Visual Studio 2022 (or Build Tools) with **Desktop development with C++** and
  **C++ Clang tools for Windows** (clang-tidy and clang-format; the build uses the copies that
  ship with the Visual Studio that provides the compiler).
- CMake 3.25 or newer and Ninja, on `PATH`.
- vcpkg. The presets use `$env:VCPKG_ROOT`; when it is unset they fall back to `C:\vcpkg`
  (`cmake/vcpkg-toolchain.cmake`). Dependency versions are pinned by `builtin-baseline` in
  `vcpkg.json`, so any vcpkg clone that contains that commit works.
- Run everything from a **developer shell** (x64), so `cl.exe` is on `PATH`:
  `Developer PowerShell for VS 2022`, or in any pwsh:

  ```powershell
  $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
  Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
  Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
  ```

## Build and test

| Preset | What it is |
|---|---|
| `debug` | Debug build with AddressSanitizer; the run that catches memory errors |
| `release` | Optimized build with PDBs; what `tools/deploy/deploy.ps1` builds and installs |

```powershell
cmake --preset debug                    # configure; the first run builds the vcpkg dependencies
cmake --build --preset debug            # build (warnings are errors)
ctest --preset debug                    # all unit tests
ctest --preset debug -L model           # one module's tests (label = module name)
ctest --preset debug -L acceptance      # the seven-prompt acceptance suite (acceptance.md)
cmake --build --preset debug --target format-check   # clang-format, fails on any difference
cmake --build --preset debug --target format         # rewrite files in the repository style
cmake --build --preset debug --target lint           # clang-tidy, fails on any finding
pwsh -File tools/dev/check.ps1          # all of the above, as the pre-commit hook runs it
```

Build trees live in `build/<preset>` (deploy contract C1). `cmake --install build/release
--prefix <dir>` produces the relocatable tree: executables and PDBs in `bin/`, the `data/`
folder of the repository in `data/` (contract C2).

## Layout

```
src/core/<module>/      pure C++20 libraries; no Win32 header (checked on every build)
src/core/model/         shared vocabulary: ids, units, Result, Finding, Plan, Action, Actor
src/core/os/            interfaces to the OS (IFileSystem, IClock, IJobControl, IProcessControl)
                        + the portable SystemClock
src/core/store/         SQLite store: migrations, synchronous and batched writes, time series
src/core/safety/        SafeRemove and its hard deny-list: the only code that deletes or moves
src/core/actions/       plan state machine, action_log/undo_blob/quarantine, DeleteItems,
                        QuarantineItems
src/core/sched/         in-app scheduler (cron, catch-up) and its store-backed repository
src/core/rules/         case schemas and parsers: cleaning rules, classes, knowledge base
src/core/packs/         knowledge packs (data/packs, docs/packs.md): loader, applies_when,
                        registry feeding every engine, relocation recipes, compress targets,
                        the fixture runner behind `myclean packs validate`
src/core/clean/         rule engine: evaluates clean cases and finds dev artifacts (git-aware)
src/core/cleanup/       Windows cleanup handlers, native prune commands and DISM as actions
src/core/space/         scanner v1 and the space analyses (top-N, old downloads, installers,
                        duplicates, near-duplicate photos by dHash)
src/core/index/         NTFS volume index from the $MFT with USN refresh and folder growth
src/core/indexer/       Windows Search scope: build-output and cache folders it indexes (from the
                        packs' attribution areas and project cases), IndexerExclusion with undo
src/core/map/           `myclean map`: attribution of every byte of a volume and how to reclaim it
src/core/placement/     drive classes and targets, relocation and recipes, hibernation, page
                        file and shadow-storage actions
src/core/compress/      compression estimates, CompressFolder (WOF) and CompactOS actions
src/core/devdrive/      Dev Drive advisor: support and Dev Drives present, active repositories
                        and package caches that would move, the owner's steps (advice only)
src/core/vhdx/          WSL and Docker virtual disks: the size report from the packs' locations and
                        the ask-tier CompactVhdx action (wsl --shutdown as the owner, then compact)
src/core/drives/        drive maintenance: TRIM/defrag history, HDD fragmentation, retrim/defrag
src/core/scanall/       `myclean scan-all`: per-drive findings grouped by action, one report
src/core/telemetry/     collectors (processes, CPU, memory, disks, volumes, GPU), the pipeline
                        that samples them, history queries and the volume fill projection
src/core/events/        process start/stop and network events: router, proc_start/net_minute,
                        the events component
src/core/health/        disk health: NVMe log / SMART interpretation, assessment, daily
                        history, endurance projection, the 10-min collector
src/core/alerts/        alert rules (data/alerts.toml) and the engine: thresholds, changes,
                        rate limit, snooze, badge
src/core/apps/          installed apps: Uninstall keys and Appx packages, last use fused from
                        UserAssist, Prefetch, BAM and proc_start with source and confidence, the
                        keep/remove verdict (Win32 reads in src/platform/apps)
src/core/backup/        backup detection: every Windows backup source, backup tools, artifacts on
                        the backup drives, last-backup age, the recommendation, the read-only
                        repository check (Win32 reads in src/platform/backup)
src/core/trust/         trust signal: signature, signer, path class, knowledge-base match
src/core/governor/      governor logic: classes, policy (data/governor.toml), pressure, ladder,
                        game mode, job placement, crash recovery (snapshot in, commands out)
src/core/games/         game detection for the governor: launcher library roots, the owner's
                        GameConfigStore, fullscreen foreground with sustained GPU use
src/core/governing/     the governor in the service: proactive build/test jobs, the ladder through
                        the job and process interfaces, game mode, crash recovery and fail-open,
                        observe/act mode, governor_* tables, the methods of ipc/governor.hpp
src/core/input/         copy/paste freeze watchdog logic, mechanism classifier, incident store,
                        the incident record sent over the pipe
src/core/elimination/   guided freeze elimination: ranked suspects, baseline and pause rates, the
                        pause keeper (suspend, or guided for a keyboard-hook owner), PauseSuspect,
                        input.pauses
src/core/ipc/           frames, the JSON-RPC dispatcher with typed parameters, the method
                        contracts (status, telemetry, disk_health, alerts, input, plans,
                        governor)
src/core/mcp/           the `myclean mcp` stdio server and the health snapshot
src/core/cleaning/      cleaning in the service: executor, scheduled auto run and purge, user
                        tasks of the tray, space analyses, the methods of ipc/plans.hpp
src/core/tray_model/    tray state, the poller, the dashboard model and the approval inbox
                        (no UI)
src/core/resident/      resident processes: per-program use from the process tables (resident_fine,
                        resident_hour), the 30-day cost report and the memory-leak detector
src/core/user_tasks/    the user-scope plans the tray runs under the owner's token: native
                        commands, user-profile cleanup handlers, relocations; their reports
src/core/security/      the intruder scan: persistence points, browser extensions and their origin,
                        network talkers over time, reverse DNS, Defender and VirusTotal verdicts
src/core/report/        the monthly report: snapshot through the service's read methods and the
                        history, diff against the previous snapshot, Markdown and HTML rendering,
                        the snapshot and report tables
src/platform/<module>/  Win32 implementations of src/core interfaces (proc, perf, disk,
                        disk_health, etw, trust, input, ipc, fs, service, scan, clean, git,
                        cleanup, settings, ntfs, compress, defrag, packs, jobs, games, image,
                        indexer, devdrive, vhdx, security)
src/svc/                myclean-svc: host, Service and its components (see below)
src/tray/               myclean-tray: tray icon, toasts, dashboard (approval inbox, Space),
                        input watchdog, the worker that runs user tasks
src/cli/                myclean: read-only CLI, `map`, `scan-all`, `packs`, `security`, `report` and
                        `myclean mcp`
tests/fakes/            in-memory fakes of the src/core/os interfaces (myclean::fakes)
tests/unit/<module>/    GoogleTest suite of one module
tests/acceptance/       the seven-prompt acceptance suite over one fixture machine (acceptance.md)
cmake/                  helper functions, lint/format targets, vcpkg toolchain
```

`src/`, `src/core/`, `src/platform/`, `tests/` and `tests/unit/` add every subfolder that has
a `CMakeLists.txt` automatically. **Adding a module never edits a shared file.**

## Adding a module and its tests

1. Create `src/core/<module>/CMakeLists.txt`:

   ```cmake
   myclean_add_library(<module>
     SOURCES
       thing.cpp
       thing.hpp
     DEPENDS
       myclean::model             # other modules: myclean::os, myclean::store, ...
       unofficial::sqlite3::sqlite3)
   ```

   This defines `myclean_<module>` with the alias `myclean::<module>`: a static library when
   SOURCES has a `.cpp`, header-only otherwise. Headers are included from the `src/` root:
   `#include "core/<module>/thing.hpp"`.

2. Create `tests/unit/<module>/CMakeLists.txt`:

   ```cmake
   myclean_add_test(<module>
     SOURCES
       thing_test.cpp
     DEPENDS
       myclean::<module>
       myclean::fakes)            # FakeFileSystem, FakeClock
   ```

   This builds `myclean_<module>_tests`, links GoogleTest's `main`, and registers every test
   with CTest under the label `<module>`.

3. Reconfigure is automatic: the next `cmake --build` sees the new folders.

A platform module is the same recipe under `src/platform/<module>/`, named
`platform_<module>` (target `myclean::platform_<module>`), with Win32 headers allowed.
An executable uses `add_executable` plus `myclean_install_executable(<target>)`, which installs
the binary and its PDB into `bin/`. In the `debug` preset (`MYCLEAN_ASAN=ON`) every executable
must also get the POST_BUILD copy of `${MYCLEAN_ASAN_RUNTIME}` next to it, as
`myclean_add_test` does. Otherwise starting it outside a developer shell raises a
"clang_rt.asan_dynamic-x86_64.dll not found" system dialog.

### Third-party libraries

Every dependency is declared in `vcpkg.json` and found once in the root `CMakeLists.txt`, so
modules only link the targets:

| Library | Target |
|---|---|
| SQLite | `unofficial::sqlite3::sqlite3` |
| toml++ | `tomlplusplus::tomlplusplus` |
| nlohmann/json | `nlohmann_json::nlohmann_json` |
| spdlog | `spdlog::spdlog` |
| tl::expected | `tl::expected` (already public through `myclean::model`) |
| WIL (platform only) | `WIL::WIL` |
| Dear ImGui (DX11 + Win32 bindings, tray only) | `imgui::imgui` |
| ImPlot (tray only) | `implot::implot` |
| krabsetw, ETW consumer (platform only; links tdh) | `krabsetw::krabsetw` |
| GoogleTest | linked by `myclean_add_test` |

Third-party code is linked statically (triplet `x64-windows-static-md`), so no third-party DLL
has to be deployed. `src/core` may include only C++ standard headers, `"core/..."` headers and
the portable libraries listed in `cmake/scripts/check-core-includes.cmake`; anything else (any
Win32 header) fails the build with the file and line.

## Code conventions

- **Namespaces:** model types live directly in `myclean` (`myclean::Plan`,
  `myclean::Result`). Every other module uses `myclean::<module>` (`myclean::os`,
  `myclean::store`); fakes use `myclean::fakes`.
- **Naming** (enforced by clang-tidy): types `CamelCase`, interfaces `IName`; functions,
  variables and enumerators `snake_case`; private members end in `_`.
- **Errors:** every fallible operation returns `Result<T>` (`Result<void>` for none), built
  with `fail(Errc::..., "message")`. `Errc` is a closed list of categories in
  `core/model/result.hpp`; put specifics in the message. Exceptions do not cross module
  boundaries; fakes may throw `std::logic_error` on misuse in test setup only.
- **Enums that are stored** have `to_string()` and `parse_<enum>()` with fixed text forms
  (`PlanState::automatic` is `"auto"`).
- **Paths** are `std::filesystem::path`. Convert to UTF-8 with `path.u8string()`, never with
  `path.string()` (that uses the ANSI code page).
- **Ids** are `Id<Tag>`; a module defines its own (`using RunId = Id<struct RunTag>;`).
- **Time:** `WallTime` for stored timestamps, `MonoTime` for intervals, both from an injected
  `os::IClock`; tests use `fakes::FakeClock` and never sleep.
- **OS access** from core only through `src/core/os` interfaces; tests use the fakes. Platform
  interfaces for later phases (process table, registry, clipboard) are added with the
  phase that needs them.

### Wiring the store, SafeRemove, actions and scheduler

The service builds these once, in this order (the actions tests do the same over fakes):

```cpp
const std::vector<store::MigrationSet> sets = {actions::action_migrations()};
auto db = store::Store::open(store::database_path(program_data), clock, sets);  // + module sets
actions::StorePlanSource plans{**db};                 // SafeRemove reads plans only from the store
auto remover = safety::SafeRemove::create(file_system, plans, *platform::protected_locations(owner));
actions::PolicyEngine engine{**db, clock};            // every transition -> action_log
const actions::Environment env{.store = db->get(), .file_system = &file_system,
                               .safe_remove = &*remover, .clock = &clock};
actions::Quarantine quarantine{env, engine};          // restore and 7-day purge
sched::StoreScheduleRepository schedule{**db};        // synchronous writes to `schedule`
```

A feature submits a plan with its automation level (`engine.submit`): `suggest` waits in
`awaiting_approval` until the owner calls `engine.approve` (approving none rejects it),
`automatic` goes straight to `approved`; an `mcp:*` proposer always waits for the owner and can
never cause `approved`. `engine.execute(plan, action, actor, auto_undo)` then runs the action
(`DeleteItems` or `QuarantineItems`, built for that plan): `applying`, apply through SafeRemove,
`applied` with the undo data committed in the same transaction, verify, and `verified` or
`rolled_back` (or an alert in the report when the result got worse and is kept). A verify that
reports `pending` (a metric measured later, such as the next logon) leaves the plan `applied`
with a `verify` entry "pending"; `engine.verify_pending` measures it again with the stored undo
data and settles it the same way (`actions::pending_verifications` lists such plans; the cleaning
worker runs them every `CleaningService::verify_interval`, and `cleaning::verification_readings`
are the alerts it raises). Item outcomes,
refusals included, are written to `action_log`, whose triggers reject any UPDATE or DELETE.
Quarantined items live in `<volume>\.myclean-quarantine\<plan id>\<item id>\<name>` (a rename on
their own volume) for 7 days; `quarantine.restore` and `quarantine.purge` act through their own
stored plans, so they too go through SafeRemove.

`engine.submit` also stores the plan's parameters (`plan_params`; the cleaning executor keeps its
`cleaning::PlanSpec` there), so the approved action is rebuilt exactly; `engine.execute` hands
apply the preview reconciled with the stored plan (stored item ids, rejected items dropped).
Only the owner may undo a `verified` plan (`verified -> rolled_back`). `QuarantineItems` takes a
`QuarantineRoot`: the cleaning executor sends items (files and folders) of a volume under 10 %
free to the overflow volume (default `L:`) and skips them when that drive is not healthy, and
skips every item while the volumes' free space cannot be read. A
folder moves there all or nothing: SafeRemove copies every file, flushes the copies once, then
removes the source through its guarded delete, and puts everything back when any step fails; a
folder holding a junction, link or cloud placeholder stays where it is. Restore moves it back
the same way. `SafeRemove` and the item actions take an optional `safety::IProgress`; the
executor's `ApplyTracker` keeps the progress `cleaning.status` reports as `applying`.

### Service components

`myclean-svc` runs these `svc::IComponent`s, built in `src/svc/main.cpp` (`make_components`) and
started in this order (stopped in reverse), so that each starts after those it uses:

| Component | Does | Methods |
|---|---|---|
| `alerts` | AlertEngine over `data/alerts.toml`; the alert sink of every producer | `alerts.*` |
| `trust` | TrustEvaluator + knowledge base (the `kb.json` of every loaded knowledge pack); signer lookup for events | (used below) |
| `telemetry` | 1 s / 60 s collectors; volume facts to `alerts` | `metrics.*`, `processes.*`, `volumes.list` |
| `disk_health` | HealthCollector on its own pipeline thread (a read may wait 60 s on WMI); health facts to `alerts` | `disks.health` |
| `events` | ETW process and network events, connection snapshots | none yet |
| `governor` | resource governor (`core/governing`): build/test trees into their jobs on the ETW start, the 1 s pressure loop and ladder on the telemetry samples, game detection and game mode, crash recovery at start and fail-open at stop; `[governor] mode` in `data/governor.toml` (`observe`, the default, records only) | `governor.*` |
| `input` | `input_incident` table for the tray's watchdog | `input.*` |
| `startup` | measures the first 2 minutes of each owner logon (`logon_impact`) and, every 10 min, records the boots Windows measured from the Diagnostics-Performance log (`boot_time`) | (read by `startup.*`) |
| `resident` | every second hands the newest process table to `resident::Tracker`: per program (the lower-case image of its trees' root) and minute, CPU time, private bytes, RAM, disk I/O and the network bytes of `net_minute`; 5-minute private bytes in `resident_fine` (2 days), hourly totals in `resident_hour` (35 days); at each closed 5-minute bucket the leak readings go to `alerts` (rule `process.memory-leak`). Reads only: it cannot act on a process | `resident.*` |
| `security` | the intruder scan (`core/security`): persistence points with Defender's and the opt-in VirusTotal verdicts, browser extensions with their origin, network talkers over time from `net_minute` and the current TCP/UDP tables; the tray's visible-window reports (`visible_window`), stored verdicts (`file_verdict`); its worker resolves reverse-DNS names and makes VirusTotal lookups only when `%ProgramData%\MyClean\security.toml` turns them on. Reads only: a Defender file scan runs with `-DisableRemediation` | `security.*` |
| `elimination` | guided freeze elimination (`core/elimination`): the pauses the cleaning executor starts for approved `PauseSuspect` plans, ticked every 5 s (period end, the spans of a guided pause), suspend pauses resumed as recorded at the start after a crash and at the stop; suspends only processes `governing::Governor::protects` leaves alone | `input.pauses`, `input.end_pause` |
| `cleaning` | rule scans (the clean cases of the knowledge packs that apply, reloaded at every scan) and plans by tier; runs approved service-scope plans and, after every scan, its auto-tier plan; auto run hourly (catch-up, waits while the system disk is under 10 % idle) and hourly quarantine purge (runs past 7 days, and at once every run on a volume under 10 % free) on the in-app scheduler; user tasks for the tray; space analyses, including the build-output and cache folders Windows Search indexes (`space.indexed_folders`, with `plans.propose_index_exclusion`); the startup items, their impact and the boot and logon times; the installed apps with their last use and verdict (`apps.list`) and their uninstall plans (`plans.propose_uninstall`, run by the tray) | `cleaning.*`, `plans.*`, `user_tasks.*`, `quarantine.*`, `space.*`, `startup.*`, `apps.*` |
| `backup` | the backup check (`core/backup`) at its start, once a day and on request: every Windows backup source (File History, system images, restore points, shadow copies, OneDrive folder backup), installed backup tools, the artifacts on the drives labelled as backup drives, the last-backup age (to `alerts`, rule `backup.age`), the recommendation when no backup exists and the git repositories of `data/workspaces.toml` with no remote, unpushed or uncommitted work (git as the owner, local refs only). Reads only: it sets nothing up | `backups.check` |
| `report` | last: the monthly report (`core/report`) on its own in-app scheduler job `monthly-report` (the 1st at 09:00, catch-up at the next start, one report per month); the snapshot through the other components' read methods called in-process, the diff against the newest earlier snapshot, `report.md` and a self-contained `report.html` in `<state>\reports\YYYY-MM`, tables `snapshot` and `report`, the reading `report.ready` to `alerts` (its toast opens the HTML in the tray). Reads only | `reports.list`, `reports.get`, `reports.compare` |

The contracts of the cleaning methods are in `src/core/ipc/plans.hpp`, the governor's in
`src/core/ipc/governor.hpp` (`governor.choose`, `.pause`, `.game_mode` and `.user_state` are
tray-only). Starting a cleaning scan (`cleaning.scan`, which deletes its auto tier at once),
approving, rejecting and undoing plans, the user tasks, recycling, quarantine restore and
snoozing or dismissing an alert are tray-only
(`ipc::Access::tray`): the service checks each request's pipe client process
(`GetNamedPipeClientProcessId`) and accepts it only from the configured `myclean-tray.exe`
(`ServiceOptions::tray_image`) running as the owner in an interactive session. A console run
(`myclean-svc --console`) executes no cleaning unless it gets `--sandbox <dir>`: the packs and
workspaces then come from `<dir>\data` (no owner packs), quarantine goes to `<dir>\quarantine`, and Windows
handlers, DISM and system settings are never applied; `--tray <exe>` names its tray.

Knowledge packs (`docs/packs.md`) are loaded by `cleaning` (with `applies_when` evaluated for the
owner through `platform::Win32Machine`), `trust` and `myclean map` / `myclean packs`, each from
`<install>\data\packs` plus `%ProgramData%\MyClean\packs` when `platform::owner_packs_folder`
finds it writable by administrators only. `status` lists every pack folder the cleaning
component loaded (`packs`: id, version, origin, status, whether it applies, why it was
rejected).

Failures stay inside a component and show in `status` (`component_health`): a data file or pack
that fails its schema, a failed disk read, ETW unavailable without elevation (listed under
`unavailable`). Only what would leave the service half-initialized fails the start (contract
C7): the store refusing a component's tables or retention, or a method that cannot be
registered. Tests drive each component on a `ComponentHarness` and the whole set in one
console-mode service (`tests/unit/svc/phase1_fakes.hpp`).

### Result type: tl::expected on C++20

`Result<T>` is `tl::expected<T, Error>` (vcpkg `tl-expected`), not C++23 `std::expected`.
The architecture fixes C++20 (section 10), and MSVC offers `std::expected` only in its
`/std:c++latest` preview mode. `tl::expected` has the same interface (`has_value`, `value`,
`error`, `and_then`, `transform`), so moving to `std::expected` later is a rename in
`result.hpp`.

## Quality gate

| Check | Where | Fails on |
|---|---|---|
| Build | every build | any MSVC warning (`/W4 /WX /permissive-`) |
| Core isolation | every build and `lint` | a non-portable include under `src/core` |
| `format-check` | hook, CI | any clang-format difference (`.clang-format`) |
| `lint` | hook, CI | any clang-tidy finding (`.clang-tidy`, warnings are errors) |
| Tests | hook, CI, deploy | any failing test; `debug` runs under AddressSanitizer |

Silence a clang-tidy finding only with `// NOLINT(<check>): <reason>` on the line.
Under AddressSanitizer the STL container annotations are disabled
(`_DISABLE_STRING_ANNOTATION`, `_DISABLE_VECTOR_ANNOTATION`) because the vcpkg libraries are
built without ASan and MSVC refuses to link mixed annotations; every other ASan check is on.

### Pre-commit hook

Enable it once per clone:

```powershell
git config core.hooksPath .githooks
```

`.githooks/pre-commit` runs `tools/dev/check.ps1`: build, `format-check`, `lint` and the full
test suite of the `debug` preset (set `MYCLEAN_HOOK_PRESET=release` to use release). The hook
checks the working tree, so stage everything you intend to commit. Never bypass it with
`--no-verify`.

### CI

`.github/workflows/ci.yml` runs on `windows-latest` for `debug` and `release`: configure,
build, format-check, lint, ctest. vcpkg is cloned at the `builtin-baseline` commit and its
binary archives are cached.
