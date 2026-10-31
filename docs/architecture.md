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

- **Classification:** every process start (ETW) is matched against
  `classes.toml` (image, command-line regex, path root, signer). The first
  match wins.
- **Proactive jobs:** each `build`/`test` start is assigned to that class's
  job object (`JobObjectCpuRateControlInformation` hard cap +
  `JOB_OBJECT_LIMIT_JOB_MEMORY`, plus notification limits at 80 %). One I/O
  completion port receives new-process, exit and limit messages.
- **Pressure loop (1 s):** inputs are CPU, available memory, commit, hard
  faults/s and the foreground window's probe latency. Pressure needs the
  threshold held for a dwell time; it clears after 60 s below the exit
  threshold.
- **Ladder:** a per-tree state machine with a 10 s cooldown per step.
  Steps 1–4 roll back on clear; a suspend waits for the owner; kill is
  opt-in.
- **Game mode:** entered on game detection. It tightens build caps and,
  optionally, moves build jobs to the frequency CCD
  (`GetLogicalProcessorInformationEx` → L3 grouping → CPU sets).
- **Fail-open:** jobs are created without `KILL_ON_JOB_CLOSE`. On a clean
  stop the service lifts every cap and resumes everything it suspended. A
  crash cannot lift caps: a job object keeps its limits while processes are
  still in it. Jobs are therefore **named** (`Global\MyClean.Job.<class>`) and
  every suspend is persisted. The restarted service (SCM recovery, 5 s)
  reopens its jobs, reconciles their limits with the current policy, and
  resumes trees whose suspend it cannot account for.

## 8. IPC and MCP

- **Pipe:** `\\.\pipe\myclean`, message mode, length-prefixed JSON-RPC 2.0.
  Methods are typed: `metrics.stream`, `metrics.query`, `inventory.get`,
  `scan.run`, `plan.get`, `plan.approve` (tray only, checked via the client
  process image and session), `governor.command`, `alerts.list`, and so on.
  No method accepts a raw path or a command line.
- **MCP:** `myclean mcp` runs a stdio server. Read tools map to read methods.
  Proposal tools (`propose_cleanup`, `propose_disable_startup`,
  `propose_uninstall`, `propose_optimization`) create plans in the
  `awaiting_approval` state, which triggers a tray notification.

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
