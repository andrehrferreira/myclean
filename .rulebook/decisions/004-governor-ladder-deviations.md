# 4. The governor's ladder differs from the PRD text (GOV-4)

**Status**: accepted
**Date**: 2026-11-05

## Context

PRD GOV-4 describes the escalation ladder for the heaviest non-protected tree as priority → memory priority → EcoQoS → cap → suspend → kill, with kill opt-in per class. In act mode on 2026-11-05, a stress test (C++ and Rust builds plus a memory hog) made the ladder suspend VS Code, Chrome, Substance Painter and Claude, picked by size, and cap Discord, Teams, ChatGPT and Docker during a build. Commit 0fd045c fixed the ladder. The owner approved the fixed ladder on 2026-11-05 and again on 2026-11-11, when the governor was allowed to act again. The P0 audit (docs/analysis/p0-audit/02-gaps.md, GOV-4) flagged the four differences as undocumented.

## Decision

- **One step for priority and memory priority.** Lowering the CPU priority and the memory priority is a single step.
- **Cap and suspend for build and test trees only, by default.** `[ladder] cap` and `suspend` in data/governor.toml list the classes they apply to, build and test by default. Any other tree stops at EcoQoS.
- **Build and test trees first.** They take the ladder before heavier trees. Another tree gets a step only when no build or test tree can take one.
- **Suspend only on a memory shortage.** Suspend and kill need a memory signal: CPU pressure alone stops at the cap. Hard faults count as pressure only while available memory is below `[pressure] hard_faults_below_available_gib`.

Kill stays opt-in per class and off for every class by default (decision 1, D5).

## Alternatives Considered

- Splitting memory priority into a step of its own, as the PRD lists it. It would be a medium change (governor commands, ladder, executor, tests), and nothing asks for it.

## Consequences

PRD GOV-4 now states the ladder as built and points here and to commit 0fd045c.
