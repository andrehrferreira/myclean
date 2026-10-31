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
- Spike notes X1, X2, X4, X5, X6, X8 under `docs/spikes/`.
