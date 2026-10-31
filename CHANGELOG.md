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
