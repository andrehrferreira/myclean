# Proposal: phase3_resource-governor

## Why
Builds and test runs sometimes take all CPU and memory and freeze the whole machine; games have the same load profile and must never be throttled. A governor that caps build/test trees at launch and handles other runaways with a reversible ladder keeps the desktop usable. Milestone M3 in docs/roadmap.md; evidence F-019..F-025.

## What Changes
Process classes as data; build/test trees placed in named job objects at launch with CPU hard cap and whole-tree commit cap plus 80 % notification limits; pressure loop with hysteresis; escalation ladder (priority, memory priority, EcoQoS, cap, suspend, opt-in kill) with automatic rollback; fail-open behavior on stop and crash; game detection and game mode; tray actions and pause. Work packages G1–G4 and spikes X4–X5 in docs/dag.md.

## Impact
- Affected specs: governor
- Affected code: src/core/governor, src/platform/{jobs,etw,perf}, src/svc, src/tray, data/classes.toml
- Breaking change: NO
- User benefit: Builds and tests can no longer freeze the machine; games are never throttled
