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
- Spike notes X1–X8 under `docs/spikes/`.

### Fixed

- The WOF test helper passed the wrong structure and compressed with LZX instead of
  XPRESS4K.
- `myclean metrics query --hours` measured its window from the system clock instead of
  the injected clock.
- CI: clang-tidy is pinned to major 19, and STL container annotations are disabled under
  ASan, so CI no longer fails on code that is clean locally.
