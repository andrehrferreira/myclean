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
    report/     monthly snapshot, its diff, Markdown/HTML rendering, snapshot/report tables
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
| Boot diagnostics | `Microsoft-Windows-Diagnostics-Performance/Operational` via `EvtQuery` (readable only by SYSTEM, elevated administrators and service logons): event 101, the boot delay Windows attributed to a program, is the impact `startup.list` shows for a startup item until MyClean has measured a logon running it (last 30 days, `DegradationTime`); event 100 gives the duration of each boot and logon (table `boot_time`) | 10 min + on demand | svc |
| Input watchdog | raw input (`RIDEV_INPUTSINK`), clipboard listener, `SendMessageTimeout` probes | events | tray |

**Boot and logon durations** come from event 100, Windows' own phase-by-phase measurement of
each boot: the boot is `BootTime` (until the system is idle after the desktop appears; split
into `MainPathBootTime` and `BootPostBootTime`), the logon is `BootUserProfileProcessingTime +
BootExplorerInitTime + BootPostBootTime`, from the owner's credentials being accepted until the
system is idle, which is where startup items run. Windows already leaves out the time the logon
screen waited for the owner, so the numbers compare from day to day. The logon time the service
reads through `WTSQuerySessionInformationW` (`svc/owner_session`) says only when a logon began,
not when it was over, and ending it would need a threshold of MyClean's own; it is used to tie
the boot to the logon MyClean measured (the first one at or after the boot's start and before
the next boot), whose first 2 minutes in `logon_impact` are the contributors. A logon that
follows no boot (signing out and in again) has no duration.

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
| Guided freeze elimination | `suspect_pause` (mode, period, end, baseline and pause incidents and hours), `suspect_pause_process`, `suspect_pause_span` | permanent |
| Resident processes | `resident_fine` (5-min private bytes per program), `resident_hour` (hourly CPU, RAM, disk, network per program) | 2 d / 35 d |
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

**Verify loop (O7):** every applied plan's `verify` result is an `action_log` entry `verify` with
the metric before and after. A metric measured over time reports `pending` with its value
before; the plan stays `applied` (shown as pending in `plans.list` / `plans.get`, never as
verified) and the cleaning worker measures it again every minute, also after a restart
(`PolicyEngine::verify_pending`), until a result settles it. Disabling startup items is measured
by the median logon of the 5 boots before the change against the boots after it (pending until
the next boot is recorded); an index exclusion by the indexer's CPU seconds per hour it ran
(`searchindexer.exe` in `resident_hour`) over up to 7 days before against the hours after (24
running hours, or 7 days); both count a difference within 10 % as unchanged and fall back to
checking that their change still holds when there is no history to compare with. Optimizations
(plan kinds `system_setting` and `compact`) that got worse are undone at once when reversible
(`rolled_back`); a worse result that is kept, and an optimization whose metric could not be
measured, stay `applied`. Each outcome but `verified` raises an alert (rules `plan.rolled-back`,
`plan.worse`, `plan.unverified` in `data/alerts.toml`; the entity is the plan id).

`SafeRemove` is the only code path that deletes or moves files. It enforces
the deny-list, never follows reparse points, skips cloud placeholders,
accepts only items that belong to a stored plan, and re-validates each item
right before acting.

Deliberate exceptions (NFR-SAFE-1): these delete without going through `SafeRemove`, each limited
to what it names:
- the Recycle Bin purge of items deleted long ago, through the shell's Recycle Bin folder
  (`IFileOperation`, `Win32RecycleBin`);
- Windows' own cleanup handlers and DISM (`HandlerCleanup`, component cleanup), which Windows
  runs on the areas it owns;
- WSL commands run inside a distribution or on it: the `/tmp` prune (`find /tmp ... -exec rm -rf`
  of entries older than 7 days) and `wsl --unregister` of a distribution only after its copy on the
  target volume is imported and checked;
- MyClean's own files: the `.myclean-new` staging file of a settings write, and a config file
  such as `.npmrc` that MyClean created and removes again on undo (`ITextFiles::remove`);
- the pruning of MyClean's own crash dumps in its dump folder.

MyClean's own-file deletes (the last two) go through `platform::remove_own_file`. It refuses a
path the deny-list protects and any reparse point, and it deletes one regular file only.

## 7. Governor (detail)

Specification: `.rulebook/tasks/phase3_resource-governor/specs/governor/spec.md`. Decisions are
pure functions in `core/governor` (snapshot in, commands out); `core/governing` applies them in
the service (`svc::GovernorComponent`), over the `core/os` job and process interfaces.

- **Mode:** `[governor] mode` in `data/governor.toml` (shipped as `observe`: act was tried on
  2026-11-04 and froze the owner's programs; it returns once the fixed ladder's recorded
  decisions are approved). `observe` records every decision in `governor_event` with detail `observe`
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
  `BELOW_NORMAL`, which its children inherit. Children the root started before its start event
  was handled did not inherit the job: those in the process table are assigned to it (and
  lowered) when the root is placed, any other one on its own start event; a protected process
  and what lies below it are left out. The first notification is recorded and the limit
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
  a threshold held for its dwell enters pressure, 60 s at every exit threshold clears it. Hard
  faults count only while available memory is below `hard_faults_below_available_gib` (16): with
  memory to spare they are file reads and page-ins after a shortage. When the ladder gets no
  snapshot for 30 s (no process table, one that stopped advancing, or a CPU or commit metric
  missing), steps 1-3 are rolled back (`telemetry_stalled`), the component reports it, and
  pressure is judged afresh once the samples are back; a suspend still waits for the owner.
- **Ladder:** on the heaviest unprotected tree, one step at a time with a 10 s cooldown:
  priority + memory priority, EcoQoS, cap (a cap job at 50 % CPU and commit + 25 %; a tree in
  its MyClean tree job has that job tightened instead), suspend (persisted in
  `governor_suspend` before it is applied, with an owner notification), kill (opt-in per
  class). Build and test trees take the ladder first; another tree gets a step only when none
  of them can. Cap and suspend apply only to the classes in `[ladder] cap` and `suspend` (build
  and test): any other tree stops at EcoQoS, so a program the owner works in is never frozen.
  Suspend and kill need a memory signal: CPU pressure alone stops at the cap.
  Steps 1-3 roll back when pressure clears; a suspend waits for the owner (Resume,
  Kill, Allow once, Always allow, through the tray), a pause or the stop. Every step and
  rollback is a `governor_event` row and a `GovernorStep` entry in `action_log`. The values
  before steps 1-2 (and the priority before a job placement) are saved in `governor_saved`
  before the change. A tree's processes keep their steps when they move to other trees (the
  root exited), and its notification stays while any of them carries one; the owner's choice
  for the old root acts on the trees they are in now. A suspended tree's notification is never
  dropped to keep the list at 32. When Windows refuses a resume the owner asked for (Resume,
  Allow, a pause), the processes stay suspended and counted, the notification keeps Resume,
  the component reports it, and the resume is retried 3 times, 10 s apart. The owner's pause
  is stored in `governor_pause`, so a restarted service keeps it until it ends.
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
  lifts and releases every job, restores the priority each placed process had before its job,
  and resumes every persisted suspend as recorded, within the 30 s stop budget. A crash leaves
  jobs (permanent) and suspend records behind: the restarted
  service (SCM recovery, 5 s) re-opens each persisted job, refuses one not owned by SYSTEM
  (`untrusted_job`), adopts one with members at the current policy's limits (its members are
  then polled, since its port is gone), releases cap jobs and empty jobs, and resumes each
  recorded (PID, creation time) exactly as often as recorded (`orphaned_suspend`); nothing
  that is not recorded is resumed. It also restores what `governor_saved` holds of each live
  process (`orphaned_state`), except that a member of a re-adopted job stays at `BELOW_NORMAL`
  until it leaves the job.

## 8. IPC and MCP

- **Pipe:** `\\.\pipe\myclean`, message mode, length-prefixed JSON-RPC 2.0
  (`src/core/ipc`). Methods are typed and registered by the service
  components; the exact JSON of each is documented in the contract headers
  of `src/core/ipc/` (`status.hpp`, `telemetry.hpp`, `disk_health.hpp`,
  `alerts.hpp`, `input.hpp`, `plans.hpp`, `governor.hpp`, `resident.hpp`, `security.hpp`,
  `reports.hpp`).
  Phase 1, the governor and the resident processes:

  | Method | Parameters | Component |
  |---|---|---|
  | `status` | none | service |
  | `metrics.latest` (with per-processor CPU) | none | telemetry |
  | `metrics.query` | metric, entity, from, to, max_points | telemetry |
  | `processes.top` (trust signal per tree) | sort, limit, qualifying | telemetry + trust |
  | `processes.explain` | pid | telemetry + trust |
  | `volumes.list` | none | telemetry |
  | `disks.health` | none | disk_health |
  | `backups.check` | refresh | backup |
  | `apps.list` | unused_days | cleaning |
  | `plans.propose_uninstall` | app, client | cleaning |
  | `system_space.status` | none | cleaning |
  | `audit.list` (tray only) | before, limit | cleaning |
  | `alerts.list` | since | alerts |
  | `alerts.badge` | none | alerts |
  | `alerts.snooze` (tray only) | rule, entity, minutes | alerts |
  | `alerts.dismiss` (tray only) | rule, entity | alerts |
  | `input.record_incident` | incident (record) | input |
  | `input.summary` | none | input |
  | `input.pauses` | none | elimination |
  | `input.end_pause` (tray only) | none | elimination |
  | `governor.status` | none | governor |
  | `governor.events` | since, limit | governor |
  | `governor.choose` (tray only) | pid, created, choice | governor |
  | `governor.pause` (tray only) | minutes | governor |
  | `governor.game_mode` (tray only) | override | governor |
  | `governor.user_state` (tray only) | state, foreground_pid | governor |
  | `resident.costs` | none | resident |
  | `resident.trend` | key | resident |
  | `security.scan` | none | security |
  | `security.extensions` | none | security |
  | `security.network` | hours | security |
  | `security.defender_scan` | item | security |
  | `security.windows` (tray only) | windows (record) | security |
  | `reports.list` | none | report |
  | `reports.get` | month | report |
  | `reports.compare` | a, b | report |

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
  own schema and the same guarantee: the `windows` of `security.windows`
  holds only process ids (`core/security/network.hpp`).
- **MCP:** `myclean mcp` runs a stdio server. Its read tools map to read
  methods: `health_snapshot` (qualifying trees with trust and the
  unrecognized ones, system signals), `metrics_latest`, `metrics_history`,
  `top_processes`, `list_volumes`, `explain_process`, `disk_health`,
  `list_alerts`, `freeze_summary`, `freeze_elimination`, the storage tools, `dev_drive_advice`, `list_vhdx`, `list_plans`,
  `get_plan`, `list_startup`, `boot_times`, `background_costs`,
  `memory_trend`, `find_indexed_build_folders` and the intruder scan's `scan_suspicious`,
  `list_browser_extensions` and `list_network_processes`, and the monthly report's `get_report` and
  `compare_snapshots`. Proposal tools (`propose_cleanup`,
  `propose_optimization`, `propose_disable_startup`, `propose_index_exclusion`,
  `propose_compact_vhdx`, `propose_pause`, `propose_uninstall`) create plans in the `awaiting_approval` state, which
  triggers a tray notification. They name what they propose only by the
  numbers a read tool returned (candidate ids, startup item ids, folder ids, disk ids, suspect
  ids, app ids), never by
  a path or registry name, so no client string reaches an OS API.

**Backup check (B1, `core/backup`, `svc::BackupComponent`):** `backups.check` reports every
backup source of spike X8 (`docs/spikes/X8-backups.md`) as found, not found or could not be checked
(with the error): File History (the owner's `Config1.xml`), system images (`WindowsImageBackup`
folders, else `wbadmin get versions` by its exit code), restore points and shadow copies (WMI,
listed but never counted: they live on the volume they protect), OneDrive folder backup (a known
folder under an account's `UserFolder`), installed backup products, and the artifacts on the volumes
whose label holds "backup" (images, `WindowsImageBackup` and `FileHistory` folders, dated manual
copy folders, dumps; a walk of depth 3 that enters no reparse point and opens no file). The newest
copy that counts gives the last-backup age and its source, reported to the alert engine once a day
(rule `backup.age`, `above` = the owner's limit in days); with no backup, a recommendation names
only healthy drives of this machine and MyClean sets nothing up. The git repositories under the
roots of `data/workspaces.toml` are checked as the owner with `remote`, `rev-list --branches --not
--remotes` and `status --porcelain`: local refs only, nothing is fetched. MCP `check_backups` reads
it.

**Installed apps (A1–A2, `core/apps`, the cleaning component):** `apps.list` reads every entry of
the Uninstall keys (HKLM 64-bit and 32-bit, the owner's HKCU) that Settings > Apps shows and the
owner's Appx packages (`PackageManager.FindPackagesForUser`, main packages only) with publisher,
version, install date, size and the knowledge-base description ("unknown" when no entry matches
its programs). Last use is the newest run among UserAssist, Prefetch (decompressed with
`RtlDecompressBufferEx`), BAM and MyClean's own `proc_start` history, named with its source and a
confidence; with no run anywhere it is "no usage recorded" and states how far MyClean's history
reaches. The verdict is `keep` for system Appx packages, runtimes and frameworks, drivers and the
apps of the computer's maker (the SMBIOS manufacturer `Win32_ComputerSystem` reports),
`removable` only when a knowledge-base entry marks the app safe to remove, `unknown` otherwise.
`plans.propose_uninstall` names an app only by its id in the latest answer and creates an
`awaiting_approval` plan of kind `uninstall` that the tray runs as the owner once approved: the
UninstallString (or QuietUninstallString) the tray reads again from the registry, or
`PackageManager.RemovePackageAsync` for an Appx package. A `keep` verdict travels with the plan as a
warning; MyClean never deletes a program folder or registry entry itself and reports a folder the
uninstaller left behind. MCP `list_installed_apps` and `propose_uninstall` map to these methods.

**Guided freeze elimination (I3, `core/elimination`, `svc::EliminationComponent`):** once the
first freeze incident is 7 days old, `input.pauses` offers the highest-ranked running suspect
(the rank of `input::known_suspects()`) not compared yet. A pause is a plan (`PauseSuspect`, kind
`pause`) that runs only after the owner approves it in the tray; an approved pause waits while
another one is active. A suspect that owns a low-level keyboard or mouse hook (PowerToys Keyboard
Manager, Armoury Crate's session helper, Discord, or any suspect running in a slow-keyboard-hook
incident) is never suspended: suspended, it would make every keystroke wait for the hook timeout
until Windows drops the hook, and it would stay broken after the resume. Its pause is guided: the
owner quits it, only the spans no process of it runs count, an alert (`freeze.guided-pause-ended`)
reminds the owner to start it again, and MyClean never launches or ends it. Any other suspect is
suspended in the owner's session only (never a service, a game, an allowed or MyClean process or
the foreground app, as `governing::Governor::protects` judges), each process recorded in
`suspect_pause_process` before its suspend and resumed exactly as recorded at the end, at the
owner's early end, at the clean stop and by a service restarted after a crash. Each pause records
its baseline (the 7 days before it, earlier pauses left out) and the incidents and hours during
it, and the plan's verification compares the two rates.

**Intruder scan (E1–E3, `core/security`, `svc::SecurityComponent`):** `security.scan` lists every
persistence point: the startup items of `core/startup` plus WMI event subscriptions
(`root\subscription`), IFEO `Debugger` values, Winlogon `Shell`/`Userinit` (HKLM and the owner's
hive) and `AppInit_DLLs`, each with its location, command, the file it runs (resolved for the
owner) and that file's trust signal. A verdict ("known dangerous") comes only from Microsoft
Defender (its `MSFT_MpThreatDetection` history, read at most every 5 minutes, or an
`MpCmdRun -Scan -ScanType 3 -File <file> -DisableRemediation` scan the owner starts for one item
of the latest answer, `security.defender_scan`) or from the opt-in VirusTotal lookup; everything
else is "suspicious because <reasons>", and Windows' defaults are "default". VirusTotal and
reverse DNS are off unless the owner turns them on in `%ProgramData%\MyClean\security.toml`
(accepted only when administrators alone can write to it); VirusTotal also needs the owner's API
key and receives only the SHA-256 of a suspicious file, at most one lookup per 15 s, results kept 7
days (`file_verdict`). `security.extensions` reads the owner's Chrome, Edge, Brave and Firefox
profiles with each extension's origin (store, external, policy, unpacked; policy and external are
"probably not installed by you"). `security.network` combines `net_minute` over the requested
hours with the current TCP/UDP tables, the trust signal of each process and whether it owned a
visible window, which the tray reports every minute (`security.windows`, kept in `visible_window`).
Reverse-DNS names are resolved on the component's worker, a few per second, bounded and cached.
Nothing here closes, removes or disables anything: a persistence point that is a startup item
names it (`startup_item`), and disabling it is a `plans.propose_startup` plan that waits for the
owner's approval in the tray.

**Tray screens (UI-3, `core/tray_model/screens`, `src/tray/*_ui.cpp`):** besides the dashboard,
Cleanup, Space, Governor and Freezes, the tray shows a screen per read feature. Each loads its
methods when the owner first opens it and on Refresh (an analysis still running is asked for again
every second), and each change it offers is a proposal that waits in Cleanup for the owner's
approval. Placement shows `system_space.status` (hibernation, the page file and the system drive's
restore-point storage, each with its state, what it holds on disk and the change
`plans.propose_optimization` would make) with a Propose button, and every relocatable cache with
its newest relocate plan. Optimize shows the applied `system_setting` and `compact` plans with their
verification, `space.indexed_folders`, `space.vhdx`, `space.dev_drive` and `resident.costs`.
Startup shows `startup.list` with each item's impact and `startup.boot_times`; ticked items are
proposed through `plans.propose_startup`. A startup item is named by its number in the latest
`startup.list` answer, so the tray lists the items again right before it proposes and sends the
numbers of that answer for the keys the owner ticked: a list another client asked for meanwhile
cannot shift them. Apps shows `apps.list` (optionally only the apps not used for 6 months) with a
Propose uninstall button (`plans.propose_uninstall`, the app named the same way: listed again, then
its number in that answer). Security shows `security.scan`, `security.extensions` and
`security.network` (24 hours); an item is labelled "known dangerous" only when it carries a
Defender or VirusTotal verdict, and a Defender scan of one item (`security.defender_scan`) starts
only on the owner's click. Backups shows `backups.check`. Audit pages through `audit.list` (tray
only: the append-only `action_log`, newest first, 50 entries a page by default, each page below the
id the previous one ended at). Settings shows what the service already reports, read only: the
governor's mode and pause, the snoozed alerts, whether VirusTotal lookups are on (the key stays in
the administrators-only `security.toml`) and the workspace roots, which `backups.check` reports with
its repository check (`repositories.roots`). Reports lists `reports.list`, shows a report's text
from `reports.get` and the changes between two snapshots from `reports.compare`.

**Monthly report (RP1–RP2, `core/report`, `svc::ReportComponent`):** job `monthly-report` (`0 9 1 * *`)
on MyClean's own scheduler (`core/sched`; no OS schedule): the 1st at 09:00 local time, one
catch-up run at the next start when MyClean was off, and one more run at the next start when a run
of this month stopped before its snapshot was stored. A month with a snapshot never runs again, so
each month gets one report at most. The component is registered last and starts its first poll 5
minutes after the start (every method is registered and disk health has its first reading). The
snapshot is read through the service's own read methods, called in-process on the dispatcher:
`startup.list` (startup items, and its auto-start services), `security.extensions`,
`security.network` (listening ports with their process, windowless talkers), `security.scan`,
`processes.top` (unsigned or invalid images among processes, persistence targets and network
talkers), `space.top` of the system volume, `space.old_downloads`, `space.duplicates` (photos) and
`space.used_installers` (started with refresh, then asked again until ready), `disks.health`,
`volumes.list` and `metrics.query` (30 daily means of `volume.free`; `cpu.utility` and
`memory.available` merged over 30 days) and `resident.costs` (30-day CPU and RAM per program: the
health part never uses one sample); governor interventions (`apply_step` and `kill` rows of
`governor_event`, observe-mode decisions counted apart), freeze incidents (`input_incident`) and the
bytes the automatic cleaning freed (`apply` entries of actor `scheduler` in `action_log`) count from
the previous snapshot (or 30 days). A category that cannot be read is stored as "not collected" with
the reason. The diff against the newest earlier snapshot lists added and removed items by key, the
growth of the top folders, each drive's status and counters, the free-space change and the counts;
the first report says it has nothing to compare and lists the baseline, a report after a gap names
the month it compares with. Markdown and HTML are rendered from the same blocks into
`%ProgramData%\MyClean\reports\YYYY-MM\report.md` and `report.html`; the HTML has inline style, an
inline SVG chart and a Content-Security-Policy of `default-src 'none'`, so it loads nothing. The
snapshot and the report are stored in tables `snapshot` and `report` (permanent), then the reading
`report.ready` (entity: the month) goes to the alert engine; a click on its toast makes the tray open
the HTML it builds from the month under `%ProgramData%`. The job calls no plan, proposal or scan
method that acts, writes nothing to `action_log` and sends nothing off the machine. `reports.get`
and `reports.compare` (MCP `get_report`, `compare_snapshots`; `myclean report get|compare`) answer
`"found": false` with the months that exist for a missing month or snapshot; `reports.list` lists
the stored reports with their files, newest first.

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
