# MyClean — Product Requirements Document

| | |
|---|---|
| Status | Draft v1, 2026-10-31 |
| Owner | Andre Ferreira (sole user) |
| Platform | Windows 11 Pro (26200+), x64, C++ |
| Evidence | [analysis/myclean-planning](analysis/myclean-planning/README.md) (findings F-001 … F-043) |

## 1. Problem

A high-end developer and game-dev workstation feels slow and unstable even
though the hardware is not the limit:

- **C: is always nearly full** (84 GB of 952 GB free) while the other seven
  drives have terabytes free. Temp files, caches and system files pile up on
  C:, and the owner cannot tell what is safe to remove.
- **Builds and tests sometimes take all the CPU and RAM** and freeze the whole
  machine. Games have the same load profile and must not be throttled.
- **Ctrl+C / Ctrl+V sometimes freezes the screen**, and no log shows why.
- **Nothing watches the eight drives** (5 internal NVMe, 1 USB SSD, 2 HDDs): not their
  health, temperature, wear, errors or free-space trend, so a failure or a
  full disk arrives without warning.
- **There is no single place** to see the state of CPU, memory, disks, startup
  programs, installed apps, security and backups, or to act on it safely.

## 2. Vision

**One resident system that monitors every CPU core, the memory, every SSD
and HDD, and the processes; explains what it sees; and keeps the machine
optimized, automatically where that is safe and with one-click approval where
it is not, with every change measured and reversible.**

## 3. Target hardware (reference machine)

| Disk | Model | Bus / type | Volume | Role today | Free |
|---|---|---|---|---|---|
| 2 | Samsung 970 PRO 1 TB | NVMe (PCIe 3) | C: "SO" | OS, profile, temp | **84 GB** |
| 4 | WD_BLACK SN7100 4 TB | NVMe | D: "SSD AR" | data | 1.0 TB |
| 7 | ROG ESD-S1C 1 TB | **USB** SSD | E: "Tools & Games" | games | 159 GB |
| 6 | Sabrent Rocket 4.0 2 TB | NVMe | F: "WS" | workspace / projects | 294 GB |
| 3 | Samsung 980 PRO 2 TB | NVMe | G: "ToS" | data | 334 GB |
| 5 | Crucial P3 4 TB | NVMe (**QLC, DRAM-less**) | W: "Devkit & Assets" | SDKs, assets | 1.76 TB |
| 0 | WD Red Plus 10 TB | SATA HDD | K: "Backup 1" | backup | 5.1 TB |
| 1 | WD Red Plus 10 TB | SATA HDD | L: "Backup 2" | backup | 5.9 TB |

CPU: Ryzen 9 7950X3D (16C/32T, two CCDs, V-Cache on one). RAM: 127 GB.
Power plan: Balanced (correct for X3D core parking; must not be "optimized"
to High Performance). TRIM: enabled. All volumes NTFS.

## 4. Goals and success metrics

| # | Goal | Metric (measured by MyClean itself) |
|---|---|---|
| G1 | Full visibility | Every CPU, memory, disk, volume, GPU and process metric visible live and with ≥ 1 year of history |
| G2 | C: never strangled | C: free ≥ 25 % sustained; regrowth sources relocated |
| G3 | No resource-exhaustion freezes | 0 commit-exhaustion events and 0 "desktop unresponsive > 2 s" events caused by builds/tests per month |
| G4 | Games untouched | 0 governor actions on a detected game process |
| G5 | Early disk-failure warning | Every SMART/NVMe health degradation alerted within 1 h of it appearing |
| G6 | Ctrl+C/V root cause | Freeze incidents attributed to a mechanism + process with evidence |
| G7 | Proven optimizations | Every applied optimization has a before/after measurement and an undo |
| G8 | Prompts answered | The 7 owner prompts pass their acceptance checks (§7) |
| G9 | Cheap to run | Service + tray < 1 % avg CPU, < 150 MB RAM total |

## 5. Non-goals

- **Registry "cleaning"** (deleting orphan registry entries): measurable
  benefit is zero and the risk is real.
- **RAM "boosters"**, standby-list purging or working-set trimming as routine
  "optimization": placebo on a 127 GB machine. Standby purge is allowed only as
  a diagnosed, on-demand action.
- **Generic tweak packs** (random registry "speed hacks", disabling services
  wholesale, High Performance plan on X3D).
- Driver updating, overclocking, fan control, RGB.
- Replacing Microsoft Defender.
- Shipping a kernel driver (see NFR-SEC-2).
- Other operating systems, other users, enterprise fleet management.

## 6. Functional requirements

Priority: **P0** = v1.0 must, **P1** = v1.0 should, **P2** = after v1.0.
Automation level per feature: `observe` (show), `suggest` (plan awaiting
approval), `auto` (runs by policy). The owner can change the level of any
feature.

### 6.1 MON — Monitoring (telemetry)

| ID | Requirement | Pri |
|---|---|---|
| MON-1 | Sample CPU every 1 s: total and per logical processor, % of max frequency (throttling), processor queue length, DPC/interrupt time | P0 |
| MON-2 | Sample memory every 1 s: available, commit charge vs limit, standby/modified lists, compressed memory, hard faults/s (`Pages Input/sec`), pool usage | P0 |
| MON-3 | Sample **every physical disk** every 1 s: read/write bytes/s, IOPS, avg latency per read/write, queue length, % idle | P0 |
| MON-4 | Sample **every volume** every 60 s: size, free, free-space trend and days-until-full projection | P0 |
| MON-5 | Process table every 1 s: CPU, private bytes, working set, I/O bytes, handles, threads; grouped into process trees | P0 |
| MON-6 | Process start/stop events in real time, with command line, parent and signer | P0 |
| MON-7 | GPU: per-engine utilization, dedicated/shared memory, per process | P1 |
| MON-8 | Network bytes per process | P1 |
| MON-9 | History: 1 s raw for 1 h, 1 min aggregates for 30 days, 1 h aggregates for 2 years | P0 |
| MON-10 | Sensor bridge: CPU/GPU temperatures and fan speeds read from HWiNFO shared memory or LibreHardwareMonitor when one of them is running (MyClean ships no driver) | P2 |

### 6.2 HLT — Disk health

| ID | Requirement | Pri |
|---|---|---|
| HLT-1 | NVMe: read the SMART/Health log (temperature, % used / wear, available spare, media errors, unsafe shutdowns, data written) for all 5 internal NVMe drives | P0 |
| HLT-2 | SATA HDD: read ATA SMART attributes (reallocated, pending, uncorrectable sectors, temperature, power-on hours, CRC errors) | P0 |
| HLT-3 | USB SSD: try SAT/vendor pass-through; if the bridge blocks it, say so and fall back to Windows reliability counters | P1 |
| HLT-4 | Store health history daily; alert on threshold **and** on change (any new media error or reallocated sector is an alert) | P0 |
| HLT-5 | Temperature alerts per drive model, from its spec | P1 |
| HLT-6 | Write-endurance projection: TBW used vs rated, years left at the current write rate | P1 |
| HLT-7 | Block automatic moves and quarantine onto a drive whose health is degraded | P0 |

### 6.3 SPC — Space analysis (all volumes)

| ID | Requirement | Pri |
|---|---|---|
| SPC-1 | Scan any volume or folder: top-N files and folders, size on disk, treemap | P0 |
| SPC-2 | Live index of every NTFS volume built from the `$MFT` and kept current by the USN journal: instant search, size per folder, growth per folder | P1 |
| SPC-3 | "What grew": folders that grew the most in 24 h / 7 d / 30 d, per volume | P1 |
| SPC-4 | Duplicates across all drives (exact by hash; photos also by perceptual hash) | P1 |
| SPC-5 | Old downloads, installers already used, large forgotten files | P0 |
| SPC-6 | Cold-data report: large folders untouched for N months on fast NVMe, as candidates to move to HDD | P1 |

### 6.4 CLN — Cleaning

| ID | Requirement | Pri |
|---|---|---|
| CLN-1 | Rule catalog as data: Windows temp/caches/dumps, browsers, Electron apps, IDEs, GPU shader caches, game launchers, package managers | P0 |
| CLN-2 | Developer artifacts in every workspace root: per-toolchain rules, git-ignored check, inactive-project check, in-use check | P0 |
| CLN-3 | Windows system areas through Windows' own cleanup handlers and DISM, never by hand | P0 |
| CLN-4 | Tiers `auto` / `ask` / `report`; `auto` runs on schedule; `ask` items wait for approval | P0 |
| CLN-5 | Quarantine with restore; same volume, or another healthy volume when space is critical | P0 |
| CLN-6 | WSL/Docker VHDX size report and compaction (`ask`) | P1 |
| CLN-7 | `Windows\Installer` orphans: move-only | P2 |

### 6.5 PLC — Placement (put data on the right drive)

| ID | Requirement | Pri |
|---|---|---|
| PLC-1 | Relocate TEMP/TMP and package-manager caches off C: through their supported settings, with undo | P0 |
| PLC-2 | Page-file placement and size, hibernation mode, restore-point storage: shown with their cost, changed only on approval | P0 |
| PLC-3 | Placement advisor: recommends which drive each workload belongs on, from drive class (NVMe, QLC, USB, HDD), health and free space. Example: build output off QLC, archives to HDD | P1 |
| PLC-4 | Dev Drive advisor: explains and guides creating a Dev Drive (ReFS + Defender performance mode) and moving active repos and caches onto it | P1 |
| PLC-5 | Move a folder between drives safely (copy → verify by hash → switch → keep the old copy until confirmed) | P2 |

### 6.6 GOV — CPU/memory governor

| ID | Requirement | Pri |
|---|---|---|
| GOV-1 | Process classes as data: system, game, allow, build, test, default | P0 |
| GOV-2 | `build`/`test` trees placed in a job object at launch, with CPU hard cap and whole-tree commit cap | P0 |
| GOV-3 | Pressure detection (CPU, available RAM, commit, hard faults) with hysteresis | P0 |
| GOV-4 | Escalation ladder for the heaviest non-protected tree: priority → memory priority → EcoQoS → cap → suspend → kill (kill opt-in per class) | P0 |
| GOV-5 | Automatic rollback of steps 1–4 after pressure ends | P0 |
| GOV-6 | Game detection (list, library paths, Game Bar store, fullscreen + GPU) and game mode | P0 |
| GOV-7 | CCD-aware placement on X3D: build jobs moved to the frequency CCD while a game runs (opt-in) | P2 |
| GOV-8 | Tray actions on every intervention: Resume / Kill / Allow once / Always allow | P0 |
| GOV-9 | Pause governor for N minutes | P0 |

### 6.7 OPT — System optimizer

Every optimization is an **action** with preview, apply, measured
verification and undo (see architecture §6).

| ID | Requirement | Pri |
|---|---|---|
| OPT-1 | Startup inventory (Run keys, Startup folders, logon/boot tasks, auto services) with a measured boot impact; disable/enable selected items | P0 |
| OPT-2 | Drive maintenance: SSD re-TRIM, HDD defragmentation, run only when idle, with the result recorded | P1 |
| OPT-3 | Search indexer scope: detect indexed build/cache folders and offer to exclude them | P1 |
| OPT-4 | Memory-leak detector: processes whose private bytes grow steadily for hours; alert with the trend chart | P1 |
| OPT-5 | Boot and logon time tracking, attributed to what slowed it | P1 |
| OPT-6 | Background-app report: resident processes with their 30-day CPU, RAM, disk and network cost | P1 |
| OPT-7 | Before/after verification for every applied optimization; auto-undo when an action is reversible and its metric got worse | P1 |

### 6.8 INP — Input and clipboard watchdog

| ID | Requirement | Pri |
|---|---|---|
| INP-1 | Measure copy latency and paste stall without installing a hook | P0 |
| INP-2 | Record incidents > 250 ms with clipboard owner, open-clipboard holder, viewer chain, foreground app, hung windows, suspect processes | P0 |
| INP-3 | Classify each incident by mechanism (delayed rendering, clipboard held open, viewer chain, slow keyboard hook, heavy payload) | P1 |
| INP-4 | Guided elimination: pause one suspect at a time, compare incident rates | P1 |

### 6.9 SEC / APP / BAK — Security, apps, backups

| ID | Requirement | Pri |
|---|---|---|
| SEC-1 | Trust signal per process: signature, signer, path class, knowledge-base match | P0 |
| SEC-2 | Persistence points: run keys, tasks, services, WMI subscriptions, IFEO, Winlogon, AppInit | P1 |
| SEC-3 | Browser extensions in all Chromium profiles and Firefox, with install origin | P1 |
| SEC-4 | Background network talkers over time | P1 |
| SEC-5 | Verdicts only from Defender and an opt-in, hash-only VirusTotal lookup | P1 |
| APP-1 | Installed apps (Win32 + Appx) with size, publisher, last used (multi-source, with confidence) | P1 |
| APP-2 | Classify each app: removable / keep (OEM, runtime, driver) / unknown; uninstall through the app's own uninstaller after approval | P1 |
| BAK-1 | Detect File History, system images, restore points, shadow copies, OneDrive backup and third-party tools; last-backup age | P1 |
| BAK-2 | Repos in workspace roots with no remote, unpushed commits or uncommitted work | P1 |
| BAK-3 | Alert when the newest backup is older than N days | P1 |

### 6.10 RPT — Scheduler, alerts, reports

| ID | Requirement | Pri |
|---|---|---|
| RPT-1 | In-app scheduler (cron-like, catch-up after downtime). No OS scheduler | P0 |
| RPT-2 | Alerts: toast, plus the tray badge; rate-limited; per-alert snooze | P0 |
| RPT-3 | Monthly report on day 1 (health, storage, security, governor, freezes, disk health, space freed, diff against last month), Markdown + HTML, toast link | P1 |
| RPT-4 | Optional email delivery (SMTP, credentials in Windows Credential Manager) | P2 |

### 6.11 UI — Interfaces

| ID | Requirement | Pri |
|---|---|---|
| UI-1 | Tray icon: state color, tooltip with CPU / RAM / commit / C: free / worst disk health | P0 |
| UI-2 | Dashboard: live charts for CPU, memory and **each disk**; volume bars; health cards per drive; top process trees | P0 |
| UI-3 | Screens: Space, Clean, Placement, Governor, Optimize, Startup, Apps, Security, Backups, Freezes, Reports, Audit, Settings | P0/P1 |
| UI-4 | Approval inbox: every pending plan with items, size, risk and tier; approve all / some / none | P0 |
| UI-5 | CLI `myclean <cmd> [--json]` for every read feature | P0 |
| UI-6 | MCP server `myclean mcp`: read tools + proposal tools (proposals need tray approval) | P0 |
| UI-7 | UI, notifications and reports in English (decided 2026-10-31); code and docs in English | P1 |

## 7. The owner's seven prompts — acceptance

| # | Prompt (short) | Requirements | Pass when |
|---|---|---|---|
| 1 | Health check: processes > 5 % CPU/RAM, explained, unknowns flagged | MON-5, SEC-1 | All qualifying trees listed with plain description, signer, trust flag; nothing closed |
| 2 | Startup programs, which are needed, disable only selected | OPT-1 | All sources listed with impact; only approved items disabled; undo works |
| 3 | Top 20 files/folders, old downloads, duplicate photos, used installers | SPC-1, SPC-4, SPC-5 | All four lists with sizes; nothing deleted without approval |
| 4 | Apps unused for 6 months, what they are, safe or keep | APP-1, APP-2 | Each app has last-used with source, description and verdict |
| 5 | Intruders: processes, extensions, background network; known dangerous? | SEC-1 … SEC-5 | Each item explained; "dangerous" only from a verdict; nothing removed |
| 6 | Backups: exist? where? last? simplest setup if none | BAK-1, BAK-2 | All sources checked; last-backup age; concrete recommendation |
| 7 | Monthly: 1 + 3 + 5 on day 1, report of changes, scheduled | RPT-1, RPT-3 | Runs on day 1 or next boot; report diffs against previous month |

Every prompt must work both in the MyClean UI and through `myclean mcp` in
Claude Code with the prompt pasted as written.

## 8. Non-functional requirements

| ID | Requirement |
|---|---|
| NFR-PERF-1 | Service + tray: < 1 % avg CPU, < 150 MB RAM; no WMI in 1 s loops |
| NFR-PERF-2 | Dashboard opens in < 1 s; live charts at 1 Hz without dropping samples |
| NFR-SAFE-1 | Every destructive action goes through one `SafeRemove` primitive with a hard deny-list; never follows reparse points; never hydrates cloud files |
| NFR-SAFE-2 | Every change is a recorded action with undo data; append-only audit log |
| NFR-SAFE-3 | AI/MCP clients can propose but never execute; approval happens in the tray |
| NFR-SEC-1 | Service IPC pipe ACL: SYSTEM + owner only; the service accepts no raw paths or commands |
| NFR-SEC-2 | No kernel driver shipped; no vulnerable-driver dependency |
| NFR-SEC-3 | No data leaves the machine except opt-in VirusTotal hash lookups and opt-in email |
| NFR-REL-1 | Service restarts on failure; crash dumps kept for MyClean itself; DB in WAL mode with integrity check at start |
| NFR-REL-2 | Governor fails open: a clean stop lifts every cap and resumes every suspend; after a crash, the service restarted by the SCM within 5 s re-adopts its named jobs and resumes orphaned suspends |
| NFR-OPS-2 | Updating a running install takes one command, needs no installer, and rolls back automatically on a failed health check ([deploy.md](deploy.md)) |
| NFR-QA-1 | `/W4 /WX`, clang-tidy clean, unit tests on all core logic, CI gate green before merge |
| NFR-OPS-1 | No OS schedules (project rule): all timing in MyClean's scheduler |

## 9. Constraints and assumptions

- Single user, single machine; built for this hardware but nothing hard-coded
  (CCDs, disks and volumes are discovered at runtime).
- Admin rights are needed once, at install. After that the service holds the
  privileges.
- Disk health on the USB SSD may be unreadable through its bridge (HLT-3).
- CPU temperature needs a driver; it is available only through the optional
  sensor bridge (MON-10).

## 10. Open decisions

| # | Decision | Recommendation |
|---|---|---|
| D1 | UI technology | **Decided 2026-10-31:** Win32 tray + Dear ImGui + ImPlot (D3D11) |
| D2 | Quarantine overflow volume | L: (HDD, healthy) |
| D3 | Workspace roots | F:, W: + confirm others |
| D4 | Default caps build/test | CPU 75 %, commit 96 GB / 64 GB |
| D5 | Kill enabled by default | No |
| D6 | VirusTotal | Off until a key is provided |
| D7 | Sensor bridge source | HWiNFO shared memory if installed |
