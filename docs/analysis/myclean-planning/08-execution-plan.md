# 08 — Execution plan

> **Superseded** by [roadmap.md](../../roadmap.md) and [dag.md](../../dag.md),
> which cover the full scope (all drives, CPU, memory, optimization). This
> file is kept as the original reasoning.

Ordered to give value early with no risk. Read-only diagnostics come first,
because they are safe, answer half the prompts, and start collecting the
data (freeze incidents, process history) that later phases need. Destructive
features come only after the safety core is tested.

## Spikes (short, throwaway experiments that settle open API questions)

Each spike ends in a short note under `docs/spikes/`. A phase does not start
until its spikes are done.

| Spike | Question | Needed by |
|---|---|---|
| S1 | Can the `IEmptyVolumeCache` handlers run from a service without UI? Which ones, and what space do they report? (F-011) | P2 |
| S2 | Job object CPU hard cap + `JOB_OBJECT_LIMIT_JOB_MEMORY` behavior on 26200; nesting under Visual Studio / Rider jobs; how builds fail at the cap (F-020) | P4 |
| S3 | `NtSuspendProcess`/`NtResumeProcess` on process trees, including UWP and protected processes (F-021) | P4 |
| S4 | Game detection: `GameConfigStore` layout, `SHQueryUserNotificationState`, PDH GPU counters on 3+ real titles (F-023) | P4 |
| S5 | `WH_KEYBOARD_LL` timeout and hook-removal behavior on 26200; raw-input latency (F-026, F-027) | P1 |
| S6 | `StartupApproved` value format; task/service disable round-trip (prompt 2) | P5 |
| S7 | OneDrive folder-backup and File History detection values (prompt 6) | P1 |
| S8 | Raw `$MFT` + USN-journal scan speed vs `FindFirstFileEx` on C: (F-017) | P6 |

## Phases

### P0 — Foundation
- Repo skeleton (F-041 layout), CMake presets, vcpkg manifest, MSVC
  `/W4 /WX`, clang-tidy, clang-format, GoogleTest, and a GitHub Actions
  quality gate.
- `myclean-core` / `myclean-platform` split with interfaces and fakes.
- `SafeRemove` + deny-list + reparse/cloud-file guards, with exhaustive
  unit tests (F-031, F-032).
- SQLite schema + migrations; TOML/JSON loaders with schema validation
  (F-042).
- **Exit:** the gate is green in CI; deny-list tests cover every entry.

### P1 — Read-only diagnostics + MCP (prompts 1, 3, 6 answerable)
- Process snapshot, tree grouping, trust signal, knowledge base v1 (F-036).
- Storage v1: top-N, old downloads, exact duplicates, used installers
  (prompt 3); dev-artifact discovery across the workspace roots, report
  only (F-012).
- Startup listing, read only (prompt 2). Backup check (prompt 6, after S7).
- `myclean watch input`: the Ctrl+C/V watchdog as a standalone command, so
  freeze data starts accumulating now (F-027, after S5).
- `myclean mcp` with the read-only tools; CLI `--json`.
- **Exit:** prompts 1, 3 and 6 pass in Claude Code (file 06); the watchdog
  has recorded at least one real incident, or a week with none.

### P2 — Cleaning engine and space recovery
- Plans, approval, quarantine, restore, audit log (F-030, F-033).
- Rule catalog v1 (F-016), dev artifacts with the git check (F-012),
  native prune commands (F-013), Windows handlers (F-011, after S1),
  `Windows\Installer` orphans move-only (F-018).
- **Relocate** (TEMP/TMP, package caches; F-014) and **Optimizations**
  (hibernate, page file, restore-point storage; F-015), both with undo.
- **Exit:** C: free space measured before and after. The target is ≥ 200 GB
  free (F-002 + F-003 + F-004 + F-005 suggest it is reachable; to be
  confirmed by measurement). Zero items touched outside a plan.

### P3 — Service, tray, IPC, scheduler
- `myclean-svc` install/uninstall, named pipe with DACL, JSON-RPC (F-040).
- Tray: status icon, notifications, approval UI, main window (UI tech per
  F-043).
- In-app scheduler with catch-up (F-039); recurring `auto` cleaning.
- MCP proposal tools → pending plans approved in the tray (F-034).
- **Exit:** a week of unattended `auto` runs with clean audit logs; an MCP
  proposal cannot execute without a tray click (tested).

### P4 — Resource governor
- ETW monitor + 1 s sampler + history aggregates (F-022, F-025).
- Process classes, proactive build/test jobs (F-019, F-020, after S2).
- Escalation ladder with hysteresis (F-021, after S3).
- Game detection and game mode (F-023, after S4); optional CCD split
  (F-024).
- **Exit:** a stress scenario (parallel C++ + Rust builds plus a memory hog
  test) keeps the desktop responsive: foreground probe latency < 100 ms, no
  paging storm, and a running game never receives an action. Service
  overhead within the F-025 budget.

### P5 — Apps, startup actions, intruder scan (prompts 2, 4, 5)
- Startup disable/enable with undo (after S6).
- Installed apps, last-used fusion, uninstall plans (F-037).
- Persistence points, browser extensions, network history, Defender and
  opt-in VirusTotal verdicts (F-038).
- **Exit:** prompts 2, 4 and 5 pass in Claude Code.

### P6 — Monthly report and scale (prompt 7)
- Snapshots, diff, Markdown/HTML report, toast; optional SMTP.
- Scanner v2 ($MFT + USN journal) if S8 justifies it (F-017).
- **Exit:** prompt 7 passes; two consecutive monthly reports with a
  meaningful diff.

## Rough sizing

| Phase | Relative size |
|---|---|
| P0 | S |
| P1 | L |
| P2 | L |
| P3 | M |
| P4 | L |
| P5 | M |
| P6 | M (L with the MFT scanner) |

Each phase becomes one Rulebook task (`.rulebook/tasks/`) with its checklist
taken from the bullets above, in dependency order.
