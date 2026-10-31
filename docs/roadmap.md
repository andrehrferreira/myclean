# MyClean — Roadmap

Seven milestones to v1.0. Each one ends with something the owner can use. The
milestones are feature gates, not strict sequential phases: work packages
start as soon as their dependencies in [dag.md](dag.md) are done, so later
milestones overlap earlier ones.

| Milestone | Outcome for the owner | Prompts that pass |
|---|---|---|
| **M0 Foundation** | Nothing visible; safe base, CI gate | — |
| **M1 See everything** | Tray + dashboard: live CPU, memory, each of the 8 disks, volumes, GPU, process trees, disk health; Ctrl+C/V freezes recorded; CLI + MCP read-only | 1 |
| **M2 Free C:** | Space analysis, cleaning (auto/ask/report), quarantine, relocation of TEMP and caches, hibernate/page-file options; approval inbox | 3 |
| **M3 Never freeze** | Governor: builds/tests capped at launch, ladder for runaways, games protected | — |
| **M4 Optimize** | Startup manager, drive maintenance, indexer scope, leak detector, boot tracking, verify loop, NTFS live index, placement + Dev Drive advisors | 2 |
| **M5 Guard** | Intruder scan, unused apps, backup check | 4, 5, 6 |
| **M6 v1.0 Report** | Monthly report with diff; all seven prompts accepted | 7 (and 1–6 re-checked) |

## Spikes

Short experiments that answer API questions before the code that depends on
them is written ("never guess API behavior"). Each one ends with a note in
`docs/spikes/Xn-<topic>.md`. All of them can start on day 0.

| Spike | Question | Unblocks |
|---|---|---|
| X1 | NVMe health log + temperature via `IOCTL_STORAGE_QUERY_PROPERTY` on all 5 internal NVMe drives; ATA SMART on the WD Reds; what the ROG USB bridge allows | H1 |
| X2 | `WH_KEYBOARD_LL` timeout and removal behavior on 26200; raw-input timing accuracy | I1 |
| X3 | Which `IEmptyVolumeCache` handlers run headless from a service, and their reported sizes | C3 |
| X4 | Job CPU hard cap + job commit cap on real MSVC/Cargo/Gradle builds; nesting under IDE jobs; `NtSuspendProcess` on trees | G1 |
| X5 | `GameConfigStore`, `SHQueryUserNotificationState`, GPU counters across 3+ installed games (incl. one on the USB SSD) | G3 |
| X6 | `StartupApproved` format; task and service disable/enable round-trip | O1 |
| X7 | Raw `$MFT` + USN-journal scan of C: vs `FindFirstFileEx`; memory per million files | S3 |
| X8 | File History, system image, OneDrive backup and third-party backup detection | B1 |

## M0 — Foundation

- **WPs:** F1, F2, F3, F4, F5, F6, F7.
- **Exit:**
  - CI runs build (`/W4 /WX`), clang-tidy and tests, all green;
  - every `SafeRemove` deny-list entry has a test, plus the reparse-point,
    placeholder and TOCTOU tests;
  - the action state machine is tested, and the audit log rejects
    UPDATE/DELETE.

## M1 — See everything

- **WPs:** R1, R2, R3, R4, R5, R6, T1–T7, H1, H2, I1, I2, K1 (spikes X1, X2).
- **The owner gets:**
  - a tray icon with live state;
  - a dashboard with a card for each drive (throughput, latency, queue, %
    idle, temperature, wear, errors, free space with a days-until-full
    projection), CPU per core with throttling, memory (commit, standby,
    hard faults), GPU, and the top process trees;
  - alerts for disk-health changes and volumes filling up;
  - the freeze log;
  - `myclean health --json` and `myclean mcp`.
- **Exit:**
  - prompt 1 passes in Claude Code;
  - all 8 drives show health (or an explicit "not readable over USB");
  - service + tray stay within the NFR-PERF-1 budget for 24 h;
  - the watchdog records at least one real incident, or a week passes
    with none.

## M2 — Free C:

- **WPs:** S1, S2, C1, C2, C3, C4, C5, P1, P2, R7, R8 (spike X3).
- **The owner gets:**
  - top-N, old downloads, used installers and duplicates on any volume;
  - developer-artifact cleanup across the workspace roots;
  - `auto` cleaning on a schedule, and `ask` items in the approval inbox;
  - quarantine with restore;
  - relocation of TEMP/TMP and the package caches;
  - hibernation, page-file and shadow-storage options;
  - MCP proposals that wait for a tray approval.
- **Exit:**
  - prompt 3 passes;
  - C: free space is measured before and after, target ≥ 25 % free
    (G2);
  - zero file operations outside a plan in the audit log;
  - one week of unattended `auto` runs.

## M3 — Never freeze

- **WPs:** G1, G2, G3, G4 (spikes X4, X5).
- **The owner gets:**
  - builds and tests capped from launch;
  - runaways handled by the ladder with automatic rollback;
  - game mode;
  - tray actions (Resume / Kill / Allow once / Always allow) and "Pause
    governor".
- **Exit:**
  - stress scenario (parallel C++ and Rust builds plus a memory-hog test):
    foreground probe latency < 100 ms and no commit exhaustion;
  - a running game receives zero actions;
  - killing the service releases every cap (fail-open test).

## M4 — Optimize

- **WPs:** O1–O7, S3, S4, S5, P3, P4, C6, I3 (spikes X6, X7).
- **The owner gets:**
  - a startup manager with measured impact;
  - re-TRIM/defrag that runs only when the drives are idle;
  - indexer exclusions for build folders;
  - leak alerts and boot/logon timing;
  - a background-app cost report;
  - a before/after record for every optimization;
  - the live NTFS index (instant "what grew");
  - a cold-data report, plus placement and Dev Drive advice;
  - VHDX compaction and guided freeze elimination.
- **Exit:**
  - prompt 2 passes;
  - every applied optimization in the audit log has a verification
    result;
  - the NTFS index stays consistent with a full rescan on C: and F:.

## M5 — Guard

- **WPs:** E1, E2, E3, A1, A2, B1 (spike X8).
- **The owner gets:**
  - a scan of persistence points, browser extensions and background
    network talkers, with verdicts from Defender (and VirusTotal if
    enabled);
  - unused apps with a last-used source and a keep/remove verdict;
  - uninstall through each app's own uninstaller;
  - the backup status of Windows backups, third-party tools and the K:/L:
    drives, plus repos with unpushed or uncommitted work.
- **Exit:** prompts 4, 5 and 6 pass.

## M6 — v1.0 Report

- **WPs:** RP1, RP2, Q1.
- **The owner gets:** on day 1 of each month (or at the next boot after),
  a report with health, storage, security, governor, freezes, disk health,
  space freed, and what changed since last month. It opens from a toast.
- **Exit:**
  - prompt 7 passes;
  - all seven prompts pass both in the UI and through MCP;
  - two consecutive monthly reports produce a meaningful diff;
  - every P0 requirement in the PRD is implemented and tested.

## After v1.0

| Item | PRD |
|---|---|
| Sensor bridge (CPU/GPU temps, fans via HWiNFO/LHM) | MON-10 |
| CCD-aware build placement on X3D | GOV-7 |
| Safe folder mover between drives (copy → verify → switch) | PLC-5 |
| `Windows\Installer` orphans, move-only | CLN-7 |
| Email delivery of reports | RPT-4 |
| MSI installer and auto-update | — |

## Working mode

- One Rulebook task per milestone (`.rulebook/tasks/`); the checklist is its
  WPs in DAG order.
- Lanes from [dag.md](dag.md#parallel-lanes) run in parallel; the spine
  (F1 → F2 → R1 → R2 → R3 → T7 → H2) gets priority because it is the
  critical path.
- A milestone closes only on its exit criteria plus the full quality gate.
