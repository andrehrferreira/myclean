# Proposal: phase6_monthly-report-v1

## Why
Prompt 7 asks for a monthly health, storage and security run with a report of what changed, scheduled inside the app (project rule: no OS schedules). This milestone also closes v1.0: all seven prompts must pass in the UI and through MCP, and every P0 requirement must be implemented and tested. Milestone M6 in docs/roadmap.md.

## What Changes
Monthly snapshots and diff (startup items, extensions, services, listening ports, unsigned binaries, top folders, disk health, governor interventions, freeze incidents, space freed); Markdown + HTML report on day 1 (or next boot) with toast link; seven-prompt acceptance suite against fixture state; P0 requirement audit and v1.0 release notes. Work packages RP1, RP2, Q1 in docs/dag.md.

## Impact
- Affected specs: reports
- Affected code: src/core/{diff,report,sched}, src/svc, src/tray, src/cli, tests/acceptance
- Breaking change: NO
- User benefit: Monthly report of what changed; v1.0 with all seven prompts accepted
