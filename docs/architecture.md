# MyClean — Architecture

Implements [prd.md](prd.md). Evidence and rationale for individual choices
are in [analysis/myclean-planning](analysis/myclean-planning/README.md); this
document is the authoritative design.

## 1. Shape of the system

MyClean is a closed loop that runs continuously:

```
   ┌──────────┐   samples/events   ┌──────────┐  findings  ┌──────────┐  plans   ┌──────────┐
   │ Collect  │ ─────────────────▶ │ Analyze  │ ─────────▶ │  Advise  │ ───────▶ │ Approve  │
   └──────────┘                    └──────────┘            └──────────┘          └────┬─────┘
        ▲                                                                              │ policy
        │                     measured effect / rollback                               ▼ or owner
   ┌──────────┐ ◀──────────────────────────────────────────────────────────────  ┌──────────┐
   │  Verify  │                                                                  │ Execute  │
   └──────────┘                                                                  └──────────┘
```

- **Collect:** telemetry and inventories from CPU, memory, every disk and
  volume, GPU, network, processes, and the system configuration.
- **Analyze:** detectors turn data into findings: pressure, a degrading disk,
  a volume filling up, cleanable items, a leak, a freeze, a suspicious item.
- **Advise:** each finding becomes a **plan**, a list of reversible
  **actions**.
- **Approve:** the feature's automation level decides. `auto` runs;
  `suggest` waits in the approval inbox; `observe` only shows.
- **Execute:** actions run in the right security context and record their
  undo data.
- **Verify:** the metric the action targeted is measured again. A worse
  result alerts the owner, or auto-undoes the action when policy allows.

## 2. Processes

```
 ┌───────────────────────────── user session ──────────────────────────────┐
 │ myclean-tray.exe (user token; HKCU Run)                                  │
 │   UI: tray icon, toasts, dashboard, approval inbox, all screens          │
 │   User-context agents: input/clipboard watchdog, HKCU / browser / Appx   │
 │   inventories, per-user cleaning (user temp, browser & package caches)   │
 │                                                                          │
 │ myclean.exe  — CLI (`--json`)  and  `myclean mcp` (stdio MCP server)     │
 └───────────────┬──────────────────────────────────────┬───────────────────┘
                 │  \\.\pipe\myclean  JSON-RPC 2.0       │
                 │  DACL: SYSTEM + owner SID             │
 ┌───────────────▼──────────────────── session 0 ────────▼───────────────────┐
 │ myclean-svc.exe (LocalSystem, auto-start, restart on failure)             │
 │   Collectors (1 s / 60 s / daily), ETW sessions, disk-health reader       │
 │   Time-series + inventory store (SQLite WAL)                              │
 │   Analyzers, Advisor, Policy engine, Scheduler                            │
 │   Governor (jobs, ladder, game mode)                                      │
 │   System-context executor (system cleaning, settings, drive maintenance) │
 └──────────────────────────────────────────────────────────────────────────┘
```

| Concern | Lives in | Why |
|---|---|---|
| Kernel ETW, disk SMART/NVMe IOCTLs, job objects on elevated processes, system settings | svc | Needs SYSTEM/admin |
| Clipboard, raw input, HKCU, browser profiles, toasts, UI | tray | Exists only in the interactive session |
| Per-user cleaning and package-manager commands | tray | Must run with the user's token, never as SYSTEM |
| Read-only queries for AI and scripts | cli / mcp | Thin clients; read from svc over IPC, or directly for user-scope data |

The tray and the CLI keep working read-only if the service is down, and they
say which features are unavailable.

## 3. Layers and modules

```
src/
  core/         pure C++20, no <windows.h>, fully unit-tested
    model/      ids, units, Sample, Series, Finding, Plan, Action, Undo
    store/      repository interfaces + SQLite impl, migrations, downsampler
    rules/      rule/class/KB schemas, loaders, evaluator
    safety/     SafeRemove policy, deny-list, path normalization
    analyze/    detectors (pressure, fill-up, health delta, leak, cold data…)
    advise/     finding → plan builders
    policy/     automation levels, approval state machine
    governor/   classification, ladder state machine (snapshot in → commands out)
    sched/      cron parser, catch-up logic
    diff/       snapshot diff
    report/     Markdown/HTML rendering
  platform/     Win32 adapters implementing core interfaces
    proc/       NtQuerySystemInformation, tokens, signatures (WinVerifyTrust)
    etw/        krabsetw sessions: Kernel-Process, Kernel-Network
    perf/       PDH queries (CPU, memory, PhysicalDisk, GPU Engine)
    disk/       IOCTL_STORAGE_QUERY_PROPERTY (NVMe health, temperature), ATA SMART, volume info
    ntfs/       $MFT reader, USN journal
    fs/         enumeration, delete, move, reparse/cloud guards
    jobs/       job objects, completion port, suspend/resume
    reg/ wmi/ msi/ shell/ tasks/ services/ clipboard/ input/ appx/ defender/
  svc/          service host, IPC server, module wiring
  tray/         UI (Dear ImGui + D3D11 per D1), tray, toasts, user agents
  cli/          commands, MCP server
data/           rules/*.toml, classes.toml, kb/*.json, drive-specs.json
tests/          unit/, fixtures/, integration/ (Windows, sandboxed)
```

Dependency rule: `core` depends on nothing platform-specific; `platform`
depends on `core` interfaces; executables wire the two together. Every
analyzer and the governor are deterministic functions of their inputs, so
they are tested by replaying recorded fixtures.

## 4. Collectors

| Collector | Source | Cadence | Context |
|---|---|---|---|
| CPU | PDH `Processor Information(*)`: `% Processor Utility`, `% of Maximum Frequency`, `% DPC Time`, `% Interrupt Time`; `System\Processor Queue Length` | 1 s | svc |
| Memory | `GlobalMemoryStatusEx`, `GetPerformanceInfo`, PDH `Memory\*` (Standby, Modified, Pages Input/sec, Pool), `Memory Compression` process | 1 s | svc |
| Physical disks | PDH `PhysicalDisk(*)`: Disk Bytes/sec, Transfers/sec, Avg. Disk sec/Read & /Write, Current Queue Length, % Idle Time | 1 s | svc |
| Volumes | `GetDiskFreeSpaceExW`, volume → disk map (`IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`) | 60 s | svc |
| Processes | `NtQuerySystemInformation(SystemProcessInformation)`: one call for all processes | 1 s | svc |
| Process events | ETW `Microsoft-Windows-Kernel-Process` | real time | svc |
| Network per process | ETW `Microsoft-Windows-Kernel-Network`; `GetExtendedTcpTable`/`UdpTable` | real time / 60 s | svc |
| GPU | PDH `GPU Engine(*)`, `GPU Process Memory(*)`, `GPU Adapter Memory(*)` | 1 s | svc |
| Disk health | NVMe: `IOCTL_STORAGE_QUERY_PROPERTY` (`StorageDeviceProtocolSpecificProperty`, Health log 0x02) and `StorageDeviceTemperatureProperty`; SATA: SMART read data via `SMART_RCV_DRIVE_DATA` / ATA pass-through; fallback `MSFT_StorageReliabilityCounter` | 10 min + daily snapshot | svc |
| NTFS index | `$MFT` read + `FSCTL_READ_USN_JOURNAL` per NTFS volume | continuous | svc |
| Sensor bridge | HWiNFO shared memory / LibreHardwareMonitor (optional) | 2 s | tray |
| Inventories | registry, Task Scheduler (read via `ITaskService`), SCM, Appx, Uninstall keys, browser profiles, backup sources | on demand + daily | svc / tray |
| Input watchdog | raw input (`RIDEV_INPUTSINK`), clipboard listener, `SendMessageTimeout` probes | events | tray |

The **sample bus** is an in-process, lock-free single-producer ring per
collector. Subscribers (UI stream, analyzers, the downsampler) read from it
without blocking collectors.

## 5. Storage

SQLite, WAL mode, `%ProgramData%\MyClean\myclean.db` (ACL: SYSTEM + Administrators
write, owner read).

| Group | Tables | Retention |
|---|---|---|
| Time series | `ts_raw` (in-memory ring, 1 h), `ts_minute`, `ts_hour` keyed by (metric, entity, time) | 1 h / 30 d / 2 y |
| Entities | `disk`, `volume`, `process_image`, `app`, `startup_item`, `extension`, `service` | current + history |
| Events | `proc_start`, `governor_event`, `input_incident`, `alert`, `health_sample` | 2 y |
| Findings & plans | `finding`, `plan`, `plan_item`, `approval` | 2 y |
| Actions | `action_log` (append-only; trigger blocks UPDATE/DELETE), `undo_blob`, `quarantine` | permanent |
| Index | `ntfs_dir_size` (folder sizes + growth), per volume | live |
| Schedule & reports | `schedule`, `snapshot`, `report` | permanent |

Writes are batched every 10 s, except action/audit writes, which are
synchronous.

## 6. Action framework

Every change MyClean can make implements one contract:

```cpp
struct Action {
  virtual ActionInfo  describe() const = 0;          // what, why, risk, tier, context (svc|user)
  virtual Preview     preview() = 0;                 // exact effect: items, bytes, settings old→new
  virtual Result<Undo> apply(const Preview&) = 0;    // re-validates, executes, returns undo data
  virtual Verification verify(const Undo&) = 0;      // re-measure the targeted metric
  virtual Result<void> undo(const Undo&) = 0;        // may be "not reversible: regenerates"
};
```

Concrete actions: `DeleteItems`, `QuarantineItems`, `RestoreQuarantine`,
`RunNativeCommand`, `RunCleanupHandler`, `SetEnvVar`, `SetToolConfig`,
`SetHibernation`, `SetPageFile`, `ResizeShadowStorage`, `DisableStartupItem`,
`SetServiceStartType`, `DisableTask`, `RetrimVolume`, `DefragVolume`,
`CompactVhdx`, `SetIndexerScope`, `LaunchUninstaller`, `GovernorStep`
(priority, EcoQoS, cap, suspend, kill), `PauseSuspect`.

The **policy engine** is a state machine over plans:
`proposed → (auto | awaiting_approval) → approved → applying → applied → verified | rolled_back | failed`.
Each transition goes to `action_log` with the actor (`scheduler`, `governor`,
`owner`, `mcp:<client>`). Actors of type `mcp:*` can create proposals but
can never trigger the `approved` transition.

`SafeRemove` is the only code path that deletes or moves files. It enforces
the deny-list, never follows reparse points, skips cloud placeholders,
accepts only items that belong to a stored plan, and re-validates each item
right before acting.

## 7. Governor (detail)

Specification: `.rulebook/tasks/phase3_resource-governor/specs/governor/spec.md`. Decisions are
pure functions in `core/governor` (snapshot in, commands out); `core/governing` applies them in
the service (`svc::GovernorComponent`), over the `core/os` job and process interfaces.

- **Mode:** `[governor] mode` in `data/governor.toml` (shipped as `act` since the owner's choice
  of 2026-11-04). `observe` records every decision in `governor_event` with detail `observe`
  and changes nothing: no priority, EcoQoS, suspend or kill, and no proactive job is created.
  `act` applies every decision and also writes each change to `action_log`. Crash recovery and the clean stop lift MyClean's own
  caps and resume its own suspends in either mode; a reload that switches from `act` to
  `observe` fails open first.
- **Classification:** every process start (ETW, through the events component's router) is
  matched against the classes of the loaded knowledge packs (image, command-line regex, path
  root, signer); the first match wins. A process in session 0 (a service) is always `system`,
  whatever matched, so the ladder never acts on a service. A detected game is `game`; the owner's Always-allow
  images are `allow`. Policy and classes are reloaded every 5 minutes; a rejected file or pack
  keeps the previous ones.
- **Proactive jobs, one per tree:** a `build`/`test` process whose parent is not in a MyClean
  tree job becomes a tree root (spike X4 §5.1). A root in no job is assigned to its class's
  parent job first, so its tree job nests under it (aggregate class caps); a root already in a
  foreign job (an IDE's) gets a tree job only, nested under that job. Each job carries the
  class's CPU hard cap (`JobObjectCpuRateControlInformation`), the commit cap of the whole tree
  (`JOB_OBJECT_LIMIT_JOB_MEMORY`) and a commit notification limit at 80 %; the root runs at
  `BELOW_NORMAL`, which its children inherit. The first notification is recorded and the limit
  cleared; the 1 s loop also checks the job's commit from its accounting and re-arms the limit
  below 70 %. One completion port per job reports membership, `JOB_MEMORY_LIMIT` and member
  exits; a tree job is released when its last member exits (a root exiting while `mspdbsrv`
  or a compiler server lives on is normal).
- **Jobs are permanent and named** `Global\MyClean.Job.<class>.<nonce>` with a random nonce
  (`OBJ_PERMANENT`: they outlive the service's handles, spike X4 §5.5), created without
  `KILL_ON_JOB_CLOSE`, `BREAKAWAY_OK` or `SILENT_BREAKAWAY_OK`, and made temporary again when
  released. Every job is persisted in `governor_job` (class, nonce, role class/tree/cap, root)
  before anything is assigned to it.
- **Pressure loop (1 s):** the telemetry pipeline's own samples (CPU, available memory, commit,
  hard faults/s and the process table; no WMI, no query of its own) feed the pressure detector:
  a threshold held for its dwell enters pressure, 60 s at every exit threshold clears it.
- **Ladder:** on the heaviest unprotected tree, one step at a time with a 10 s cooldown:
  priority + memory priority, EcoQoS, cap (a cap job at 50 % CPU and commit + 25 %; a tree in
  its MyClean tree job has that job tightened instead), suspend (persisted in
  `governor_suspend` before it is applied, with an owner notification), kill (opt-in per
  class). Steps 1-3 roll back when pressure clears; a suspend waits for the owner (Resume,
  Kill, Allow once, Always allow, through the tray), a pause or the stop. Every step and
  rollback is a `governor_event` row and a `GovernorStep` entry in `action_log`.
- **Game mode:** a game is on the owner's list, under a launcher library root, path-matched in
  the owner's `GameConfigStore` (read from `HKU\<owner SID>`), or the fullscreen foreground
  process with sustained GPU use (the tray sends the notification state and foreground PID
  every second; GPU engines are sampled only while there is a candidate). Game mode (or the
  tray's override) lowers every build job's CPU cap to 40 % and shortens dwell and cooldown to
  5 s; a game and its descendants are never acted on.
- **Orphaned polling loops:** every 10 s the process table is searched for shell, interpreter
  and polling-tool trees whose creator is gone (PID reuse checked), at least 10 minutes old,
  grouped by normalized command line. A pattern with 3+ live trees is offered in the tray (End
  now, Always end this pattern, Ignore; `governor.orphans`); rules persist in
  `governor_orphan_rule`, and later orphans of an "always" pattern are ended without asking.
  Protected classes, the foreground process and MyClean are never ended; each end is a kill
  event with reason `orphaned_loop`.
- **Fail-open:** on a clean stop the service rolls back every step, resumes every suspend,
  lifts and releases every job and resumes every persisted suspend as recorded, within the
  30 s stop budget. A crash leaves jobs (permanent) and suspend records behind: the restarted
  service (SCM recovery, 5 s) re-opens each persisted job, refuses one not owned by SYSTEM
  (`untrusted_job`), adopts one with members at the current policy's limits (its members are
  then polled, since its port is gone), releases cap jobs and empty jobs, and resumes each
  recorded (PID, creation time) exactly as often as recorded (`orphaned_suspend`); nothing
  that is not recorded is resumed.

## 8. IPC and MCP

- **Pipe:** `\\.\pipe\myclean`, message mode, length-prefixed JSON-RPC 2.0
  (`src/core/ipc`). Methods are typed and registered by the service
  components; the exact JSON of each is documented in the contract headers
  of `src/core/ipc/` (`status.hpp`, `telemetry.hpp`, `disk_health.hpp`,
  `alerts.hpp`, `input.hpp`, `plans.hpp`, `governor.hpp`). Phase 1 and the governor:

  | Method | Parameters | Component |
  |---|---|---|
  | `status` | none | service |
  | `metrics.latest` (with per-processor CPU) | none | telemetry |
  | `metrics.query` | metric, entity, from, to, max_points | telemetry |
  | `processes.top` (trust signal per tree) | sort, limit, qualifying | telemetry + trust |
  | `processes.explain` | pid | telemetry + trust |
  | `volumes.list` | none | telemetry |
  | `disks.health` | none | disk_health |
  | `alerts.list` | since | alerts |
  | `alerts.badge` | none | alerts |
  | `alerts.snooze` | rule, entity, minutes | alerts |
  | `alerts.dismiss` | rule, entity | alerts |
  | `input.record_incident` | incident (record) | input |
  | `input.summary` | none | input |
  | `governor.status` | none | governor |
  | `governor.events` | since, limit | governor |
  | `governor.choose` (tray only) | pid, created, choice | governor |
  | `governor.pause` (tray only) | minutes | governor |
  | `governor.game_mode` (tray only) | override | governor |
  | `governor.user_state` (tray only) | state, foreground_pid | governor |

  Tray-only methods are checked via the client process image and session. Later phases add
  `scan.run` and so on (the cleaning methods are in `plans.hpp`).
- **Parameters** are integers, numbers, booleans or tokens (ASCII letters,
  digits, `_`, `.`, `-`; at most 128 characters), so no method accepts a raw
  path or a command line. An undeclared parameter, a missing one or a value
  of the wrong type is refused with -32602 before anything runs. Entities
  (volumes, drives, processors) cross the pipe in token form (`C:` is `C`)
  and the service maps them back.
- **The record exception:** `ParamType::record` carries data the tray
  measured that cannot be tokens, today only the `incident` of
  `input.record_incident` (process image names, clipboard format names). A
  record is a JSON object held to generic bounds (depth 4, 32 members per
  object, 64 elements per array, every string printable and at most 260
  bytes) and to a strict schema of its method
  (`core/input/incident_record.hpp`: every member known and typed, unknown
  members rejected), both checked before the handler runs. Its strings are
  stored and displayed as data only and are never passed to a file,
  process, registry or any other OS API. A new record parameter needs its
  own schema and the same guarantee.
- **MCP:** `myclean mcp` runs a stdio server. Its read tools map to read
  methods: `health_snapshot` (qualifying trees with trust and the
  unrecognized ones, system signals), `metrics_latest`, `metrics_history`,
  `top_processes`, `list_volumes`, `explain_process`, `disk_health`,
  `list_alerts`, `freeze_summary`. Phase 1 has no tool that changes
  anything. Proposal tools (`propose_cleanup`, `propose_disable_startup`,
  `propose_uninstall`, `propose_optimization`) arrive with the action phases
  and create plans in the `awaiting_approval` state, which triggers a tray
  notification.

## 9. Security

- The service is the elevation boundary: the pipe DACL allows only SYSTEM and
  the owner, methods are typed, plans are created server-side, and every
  item is re-validated at execution time.
- Rule, class and knowledge-base files under `%ProgramData%` are writable by
  Administrators only.
- No kernel driver. No network calls except the opt-in ones (VirusTotal,
  SMTP).
- MyClean's own binaries are signed (self-signed certificate is acceptable
  for single-machine use) and verified by the service at connect time.

## 10. Technology

| Concern | Choice |
|---|---|
| Language / toolchain | C++20, MSVC, CMake + Ninja, `CMakePresets.json`, vcpkg manifest |
| Win32 helpers | WIL; C++/WinRT for Appx, toasts and storage APIs |
| ETW | krabsetw |
| DB | SQLite (WAL) |
| Config / data | toml++, nlohmann/json |
| Hashing | BLAKE3, xxHash3 |
| UI | Dear ImGui + ImPlot (live charts) on D3D11; Win32 `Shell_NotifyIcon` (D1) |
| Logging | spdlog (rotating) |
| Tests | GoogleTest; fixture replay for analyzers and the governor |
| Quality | `/W4 /WX /permissive-`, clang-tidy, clang-format, ASan in debug test runs |
| CI | GitHub Actions `windows-latest`: configure → build → clang-tidy → unit tests → integration tests |

## 11. Deployment

There is no installer before v1.0. `tools/deploy/deploy.ps1` builds, runs
the test gate and hot-swaps versioned builds under
`C:\Program Files\MyClean\versions\`, behind a `current` junction, with
health checks and automatic rollback. The service is registered once
against the junction path; the tray autostarts through HKCU `Run`. The
binaries must honor the contracts C1–C7 in [deploy.md](deploy.md).

## 12. Testing strategy

| Level | What | How |
|---|---|---|
| Unit | everything in `core` | fakes for every platform interface |
| Replay | analyzers, governor, advisor | recorded time series and process trees from the real machine (`tests/fixtures`), with expected findings and actions |
| Adapter integration | `platform/*` | sandbox directories under `%TEMP%\myclean-it`; job objects on spawned dummy workloads; PDH/ETW smoke tests |
| Stress | governor end to end | synthetic parallel builds + memory hog; assert foreground probe latency < 100 ms and no commit exhaustion |
| Prompt acceptance | the 7 prompts | MCP tool calls against fixture state; manual pass in Claude Code before each release |
| Safety | `SafeRemove` | one test per deny-list entry, plus reparse-point, placeholder and TOCTOU-swap tests |
