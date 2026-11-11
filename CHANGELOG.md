# Changelog

All notable changes to MyClean are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Planning documentation: product requirements (`docs/prd.md`), architecture
  (`docs/architecture.md`), roadmap M0–M6 (`docs/roadmap.md`), work-package
  dependency graph (`docs/dag.md`) and the initial analysis with machine
  measurements (`docs/analysis/myclean-planning/`).
- Developer deployment script `tools/deploy/deploy.ps1`: builds, runs the test
  gate and hot-swaps versioned builds of the service, tray app and CLI, with
  health checks and automatic rollback (`docs/deploy.md`).
- Rulebook tasks `phase0_foundation` … `phase6_monthly-report-v1`, one per
  milestone, with requirement specs.
- MIT license.
- **Foundation (M0):** CMake/vcpkg build with debug (ASan) and release presets, CI (test,
  lint, format) and a tag-only release workflow; core model; SQLite store with additive
  migrations and time series; schema-validated loaders for rules, process classes and the
  knowledge base; `SafeRemove` with a compiled-in deny-list, reparse/cloud guards and
  identity-checked deletes; action framework with an owner-only approval state machine,
  append-only audit log and quarantine; in-app cron scheduler with catch-up.
- **See everything (M1, code complete):**
  - `myclean-svc` Windows service with a named-pipe JSON-RPC API restricted to SYSTEM and
    the owner.
  - Telemetry for CPU, memory, every physical disk and volume, GPU and process trees, with
    2-year history.
  - ETW process and network events.
  - NVMe/SMART disk health with alerts.
  - Process trust signal and a 152-entry knowledge base.
  - Alert engine.
  - Hook-free Ctrl+C/Ctrl+V freeze watchdog.
  - `myclean` CLI and a read-only MCP server.
  - Tray app with an ImGui/ImPlot dashboard.
- **Free C: (M2, code complete):**
  - Scanner v1: parallel listing with size on disk; never follows links or opens cloud
    placeholders; counts hard links once. Read-only analyses: top files and folders, old
    downloads with their origin, installers already used, exact duplicates.
  - Cleaning rule engine and catalog (72 rules) for Windows, browsers, Electron apps,
    IDEs, GPU shader caches, game launchers and package managers. Developer artifacts are
    proposed only when git ignores them, the project is inactive and nothing uses them.
  - Windows cleanup handlers (all 31 `VolumeCaches` handlers), allow-listed native prune
    commands run with the user's token, and DISM component cleanup (never `/ResetBase`).
  - Relocation of TEMP and package caches (Gradle, Cargo, npm, NuGet, pip, Go, Unreal DDC)
    through each tool's own setting, with exact undo. Only healthy drives are targets.
    Hibernation, page file and shadow storage are approval-only options.
  - Cleaning executor in the service, daily auto-tier cleaning with catch-up, quarantine
    with a 7-day purge, and the plan IPC (approve, reject, undo and restore are tray-only).
  - Tray approval inbox and Space views; approved per-user plans run with the owner's
    token and are undone automatically when verification is worse.
  - MCP tools `storage_top`, `find_old_downloads`, `find_duplicates`,
    `find_used_installers`, `list_plans`, `get_plan`, `propose_cleanup` and
    `propose_optimization`. They only propose; they never approve.
- **Machine-wide scan (M2b, code complete):**
  - NTFS volume index read from the `$MFT` in one pass, refreshed from the USN journal,
    with daily folder snapshots for growth over 24 h / 7 d / 30 d. Falls back to
    scanner v1 when not elevated.
  - `myclean map [<volume|path>]`: explains where a volume's space went, by area, with how
    each area can be reclaimed and the bytes the scan could not see.
  - Drive maintenance analysis plus `RetrimVolume` / `DefragVolume` actions. They are
    refused on the wrong medium, on an unhealthy drive, on battery or when the disk is busy.
  - Compression estimates per WOF algorithm, plus `CompressFolder` and `CompactOs`
    actions with exact undo.
  - `myclean scan-all`: one report for every fixed drive, grouped by action (remove,
    deduplicate, relocate, archive cold data, compress, defrag/TRIM, uninstall review,
    manual review). Each byte is counted once. Write actions on drives whose health is
    not good are held for manual review.
- **Knowledge packs:** all cleaning rules, disk-map areas, relocation recipes,
  compression targets and the process knowledge base live in 34 packs under
  `data/packs/`. Each pack has its own fixtures. Owner packs in
  `%ProgramData%\MyClean\packs` override built-ins. `myclean packs list | validate`.
  Guide: `docs/packs.md`.
- **Drive-wide rules (M2e):**
  - Clean targets accept `?:\` (every fixed volume); rules may opt in to projects outside
    git (`require.outside_git`) and select only folders holding a file (`require.contains`).
  - New cases: Visual Studio IntelliSense caches, Zig project caches, pip uninstall
    leftovers, idle CMake build trees of active projects, Unreal cooked/staged output (ask),
    Steam's shader pre-cache in every library (`%STEAM_LIBRARY%`, read from
    `libraryfolders.vdf`),
    and the Recycle Bin items deleted more than 30 days ago (auto: sized from the `$I`
    records by the service, deleted through the shell by the tray as the owner).
  - The service runs git as the owner (session token and environment), so project
    artifacts inside repositories are proposed again; it never sets `safe.directory`.
  - `myclean map` and `scan-all` name the artifact folders the project rules find.
  - `scan-all` finds archives already extracted on a drive (zip incl. ZIP64, RAR4, RAR5;
    manual review) and merged, clean, idle git worktrees (`git worktree remove`), and
    proposes the parts of a cold tree that mixes backups with other content apart.
  - A copy of Docker Desktop's data folder that its settings no longer name is offered for
    deletion (ask; `%DOCKER_WSL_DATA%` and `require.outside_of` keep the live one out).
  - MyClean records its own CPU and memory (`self.cpu`, `self.memory`) for NFR-PERF-1.
  - New attribution areas: custom Docker data disks, SDKs under `?:\LLVM`, databases
    (MongoDB, PostgreSQL, pgAdmin), the Megascans library, installer rollback folders.
  - Every executable carries the application icon; the tray shows it with the state as a
    dot.
- **Owner decisions (M2d):** the tray's Space view lists "Your decisions" from the
  service (`space.owner_decisions`): folders of the owner's profile on an SSD that nothing
  has written for six months, with the HDD archive folder each would move to, and the
  installed programs whose install folder lies in an area the packs mark for uninstall
  review, with their sizes. Each row has one button, and the click is the owner's approval
  of exactly that plan (`plans.owner_action`, tray only), run by the tray with the owner's
  token: "Mover para o HD" copies the folder to `<HDD>\Archive\<letter>\<path>`, checks
  the copy holds the same files and bytes and only then removes the folder through
  SafeRemove (refused while a program uses it; undo copies it back); "Desinstalar" starts
  the program's own uninstaller and reports "launched; finish in the uninstaller" until its
  Uninstall entry is gone. MCP tool `find_owner_decisions` lists them; no tool acts on them.
- **Resource governor (M3):**
  - The tray's Governor section: the governor's mode and pressure, every notification
    waiting for the owner with Resume, Kill (confirmed), Allow once and Always allow, Pause
    governor for N minutes (or end the pause) and the game-mode override auto / on / off.
    New notifications become toasts, at most one per tree per 5 minutes (a suspend or kill
    always), quiet during the owner's quiet hours; clicking one opens the section.
  - The tray reports the owner's notification state and foreground process every second
    (`governor.user_state`), which protects the foreground program and feeds fullscreen
    game detection.
  - Orphaned polling loops: shell, interpreter and polling-tool trees whose launching
    session is gone and whose command line repeats (3+ trees, 10+ minutes old) are proposed
    in the tray as "End N loops of <pattern>" with End now, Always end this pattern and
    Ignore. Patterns set to always are ended as they appear; every end is recorded
    (`governor.orphans`, `[orphans]` in `data/governor.toml`, table
    `governor_orphan_rule`).
  - A process in session 0 (a service) is always of
    class `system`: in observe mode the ladder had chosen `vmms.exe`, `NVDisplay.Container.exe`
    and vendor services. Security suites, WSL/Hyper-V, WMI hosts, database servers and remote
    access tools are listed as `system` in the process classes (pack 1.1.0).
- **Optimize (M4):**
  - Startup items: every automatic start source (Run and RunOnce in both registry views,
    Startup folders, logon and boot tasks, auto-start services not signed by Microsoft
    Windows) with its enabled state from StartupApproved (`startup.list`, MCP
    `list_startup`). `plans.propose_startup` and MCP `propose_disable_startup` take only
    the numbers of that list and create a plan that waits for the owner's approval; the
    service then disables each item the way Task Manager does (StartupApproved, the task's
    Enabled flag, a service's start type set to Manual), deletes nothing, and undo restores
    the exact state before. MCP `propose_enable_startup` turns one back on the same way.
  - Boot and logon durations per boot (`startup.boot_times`), from the Diagnostics-Performance
    log when Windows records it.
  - Resident programs: what each costs over 30 days (`resident.costs`, MCP
    `background_costs`) and an alert when a program's memory keeps growing for 6 hours
    (`process.memory-leak`).
  - Near-duplicate photos (WIC decode, 64-bit dHash), Windows Search scope for build output
    and caches (exclusion on approval), Dev Drive advice, and WSL/Docker virtual disks
    compacted on approval.
  - Every applied optimization is measured before and after; one that made things worse is
    undone automatically and reported.
  - Guided freeze elimination: with the owner's approval, one suspect at a time is paused and
    the freeze rates before and after are compared.
- **Guard (M5):**
  - Intruder scan (`security.scan`, MCP `scan_suspicious`): every persistence point (startup
    items, WMI event subscriptions, IFEO Debugger, Winlogon Shell/Userinit, AppInit_DLLs)
    with the file it runs and that file's trust signal.
  - Browser extensions of every Chrome, Edge, Brave and Firefox profile with their install
    origin (MCP `list_browser_extensions`); policy-forced and external ones are flagged
    "probably not installed by you".
  - Background network talkers over time with trust and whether they had a visible window
    (MCP `list_network_processes`); reverse DNS is off by default.
  - Verdicts come only from Defender's detection history, a Defender scan of one item the owner
    asks for, or the opt-in, hash-only VirusTotal lookup (off until a key is put in the
    administrators-only `security.toml`). Everything else is "suspicious because …".
  - Installed apps (Uninstall keys and Appx) with size, publisher and last use from UserAssist,
    Prefetch, BAM and MyClean's own history, each with its source; a keep / removable /
    unknown verdict; an uninstall plan runs the app's own uninstaller after tray approval
    (MCP `list_installed_apps`, `propose_uninstall`).
  - Backup check (MCP `check_backups`): File History, system images, restore points, shadow
    copies, OneDrive folder backup, backup products and dated copies on backup volumes; the
    last-backup age with an alert after 7 days; a recommendation naming healthy drives when
    none exists; git repositories with no remote, unpushed or uncommitted work.
  - CLI `myclean security`, `myclean apps` and `myclean backups`, each with `--json`.
- **Monthly report (M6):**
  - On the 1st at 09:00, or at the next service start when the PC was off, at most once a
    month, through MyClean's own scheduler: a snapshot of startup items, extensions,
    services, listening ports, unsigned binaries, top folders, disk health, the C: free-space
    trend, governor interventions, freeze incidents and space freed, with health from
    30-day aggregates. A category that cannot be collected says why.
  - The report lists what changed since the newest earlier snapshot, and is written as
    Markdown and a self-contained HTML file under `%ProgramData%\MyClean\reports\YYYY-MM\`;
    a toast opens it. The run is read-only and sends nothing off the machine.
  - `reports.list`, `reports.get`, `reports.compare`; MCP `get_report`, `compare_snapshots`;
    `myclean report get|compare`.
- **Tray screens:** Placement, Optimize, Startup, Apps, Security, Backups, Audit (the
  append-only action log, read only), Settings and Reports, besides the dashboard, Cleanup,
  Space, Governor and Freezes. Every change they offer is a plan that waits for approval.
- **Acceptance suite** (`tests/acceptance`, `docs/acceptance.md`): the owner's seven prompts
  asked through the MCP server against one fixture machine, each checked against its pass
  condition, and the audit log checked for any action without a tray approval.
- **P0 audit** (`docs/analysis/p0-audit/`): every P0 requirement mapped to its implementation
  and tests, with the gaps and their fixes.
- Owner decisions recorded under `.rulebook/decisions/`: automatic system-space recovery when
  C: is critically low, the governor's ladder (cap and suspend only build and test trees), and
  automatic removal of idle, git-ignored node artifacts.
- Spike notes X1–X8 under `docs/spikes/`.

### Fixed

- The quality gate runs at below-normal priority on half the logical processors: at the
  default it started one compiler or clang-tidy per processor and held the owner's CPU at
  100 %.
- The tray's once-a-second `metrics.latest` no longer recomputes each volume's days-until-full
  projection over a week of minute rows; it is computed once per 60 s volume sample.
- Governor: children a build started before its root was placed now join its job; steps are
  rolled back when telemetry stalls for 30 s; saved priorities, memory priorities, EcoQoS and
  the pause survive a crash or restart; a suspended tree never loses its Resume, and a resume
  Windows refuses is retried and shown.
- Report-view items sent to the Recycle Bin are carried out by the tray instead of stopping at
  an unreadable task; `alerts.snooze`, `alerts.dismiss` and `cleaning.scan` are tray-only;
  quarantine never falls back to a critically low volume; signers resolved after a process
  started are stored, and process stops are recorded.
- The governor acts again (mode `act`, approved by the owner on 2026-11-11): the decisions
  recorded in observe mode since 2026-11-10 capped only build and test trees.
- The governor no longer freezes programs the owner works in: a stress test on the reference
  machine had it suspend VS Code, Chrome, Substance Painter and Claude, chosen by size while
  a memory hog elsewhere caused the pressure. Cap jobs and suspends now apply only to the
  classes listed in `[ladder] cap` and `suspend` (build and test by default); other trees stop
  at EcoQoS. Hard faults count as pressure only while available memory is below
  `[pressure] hard_faults_below_available_gib` (16): with 76 GiB free, page-ins after the hog
  exited had kept the governor in pressure and stepping. Build and test trees take the ladder
  first (a build had Discord, Teams and ChatGPT capped, each heavier than one compiler), and
  only a memory shortage suspends or kills. The governor ships in observe mode again until its
  recorded decisions are approved.
- The WOF test helper passed the wrong structure and compressed with LZX instead of
  XPRESS4K.
- `myclean metrics query --hours` measured its window from the system clock instead of
  the injected clock.
- CI: clang-tidy is pinned to major 19, and STL container annotations are disabled under
  ASan, so CI no longer fails on code that is clean locally.
- CI: the core include check sets its CMake policies (it failed on the runner's CMake), and
  the CPU-share test allows for clock-tick rounding on a 4-processor runner.
- `myclean map` spells its root with long names: under an 8.3 path such as
  `C:\Users\RUNNER~1` nothing was attributed.
- Rules acting inside active projects count only a process inside the item as using it, not
  one anywhere in the project (idle terminals kept every incremental cache).
- `zig/global-cache` deletes only the `h`, `z` and `tmp` entries, never `o` entries one by
  one.
