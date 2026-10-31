# MyClean — planning analysis

MyClean is a C++ Windows 11 utility in the spirit of CCleaner, built for one
developer workstation. It has four jobs:

1. keep C: free through recurring, safe cleaning and relocation;
2. a tray daemon that stops builds and tests from taking the whole machine,
   without ever throttling games;
3. find the cause of the Ctrl+C / Ctrl+V freezes;
4. answer seven health, security and maintenance prompts, both in its own UI
   and through an MCP server for AI clients.

Analysis date: 2026-10-31. Measurements come from the target machine.

## Executive summary

- **C: is the only volume in trouble** (84 GB free of 952 GB, against
  159 GB–5.9 TB free elsewhere). About **112 GB** is the hibernation file and
  the page file, **13.5 GB** is the user temp folder, about **21 GB** is
  Windows-managed caches and about **9 GB** is developer caches. Most of this
  can be **relocated or turned off** instead of being cleaned again every
  month. → [01](01-context-and-measurements.md), [02](02-disk-cleaning-and-relocation.md)
- **Recurring cleaning is safe only when it is tiered:** `auto` for
  regenerable data (temp, caches, build outputs that git ignores in inactive
  projects), `ask` for the rest (quarantined, restorable), `report` for
  personal files. → [02](02-disk-cleaning-and-relocation.md), [05](05-safety-model.md)
- **The governor caps builds at launch** with Windows job objects (CPU hard
  cap plus a whole-tree memory cap). It treats any other runaway with a
  reversible ladder: priority → efficiency mode → cap → suspend, with kill
  opt-in. Games are detected and protected; on the 7950X3D, builds can be
  moved off the V-Cache CCD while a game runs. → [03](03-resource-governor.md)
- **The Ctrl+C/V freeze has 5 possible mechanisms**, and they can be told
  apart by a user-mode watchdog that adds no hook of its own. Today's top
  suspects are the PowerToys Keyboard Manager (low-level keyboard hook) and
  AnyDesk (clipboard sync). → [04](04-input-and-clipboard-freezes.md)
- **The seven prompts become testable through `myclean mcp`**. AI clients
  can read and *propose*; only a click in the tray executes anything. The
  monthly job runs on MyClean's own scheduler, as the project rules require.
  → [06](06-assistant-prompts.md)
- **Architecture:** service (SYSTEM) + tray (user) + CLI/MCP, pure core logic
  behind Win32 adapters, rules as data files. → [07](07-architecture.md)
- **Plan:** P0 foundation → P1 read-only diagnostics + MCP → P2 cleaning →
  P3 service/tray → P4 governor → P5 apps/security → P6 monthly report.
  → [08](08-execution-plan.md)

## Index

| File | Theme | Findings |
|---|---|---|
| [01-context-and-measurements.md](01-context-and-measurements.md) | Reported problems, machine snapshot | F-001 – F-010 |
| [02-disk-cleaning-and-relocation.md](02-disk-cleaning-and-relocation.md) | Cleaning catalog, relocation, scanner | F-011 – F-018 |
| [03-resource-governor.md](03-resource-governor.md) | CPU/RAM governor, game protection | F-019 – F-025 |
| [04-input-and-clipboard-freezes.md](04-input-and-clipboard-freezes.md) | Ctrl+C/V freeze diagnosis | F-026 – F-029 |
| [05-safety-model.md](05-safety-model.md) | Quarantine, deny-list, privilege, consent | F-030 – F-034 |
| [06-assistant-prompts.md](06-assistant-prompts.md) | The seven prompts, MCP, acceptance | F-035 – F-039 |
| [07-architecture.md](07-architecture.md) | Processes, stack, layout, storage | F-040 – F-043 |
| [08-execution-plan.md](08-execution-plan.md) | Spikes and phases | — |

## Things the owner can do today, without MyClean

1. **Freeze test:** quit PowerToys Keyboard Manager (or remove its Ctrl remaps)
   and close AnyDesk for a day. Note whether the Ctrl+C/V freezes stop (F-029).
2. **Hibernation file:** if full hibernate is never used,
   `powercfg /h /type reduced` (keeps Fast Startup) or `powercfg /h off`
   frees most of the 50.9 GB (F-002).
3. **TEMP off C:** point the user `TEMP` and `TMP` variables at a folder on
   another SSD (F-003).

## Open decisions (owner)

| # | Decision | Recommendation |
|---|---|---|
| D1 | Main-window UI technology (F-043) | Win32 tray + Dear ImGui |
| D2 | Quarantine volume when C: is critically low (F-030) | L: "Backup 2" |
| D3 | Workspace roots to scan for projects (F-012) | F:, W: (confirm, and add others) |
| D4 | Default governor caps for `build`/`test` (F-020) | CPU 75 %, commit 96 GB / 64 GB |
| D5 | Kill enabled by default for any class? (F-021) | No |
| D6 | Optional VirusTotal hash lookup (F-038) | Off until an API key is provided |
| D7 | UI language | **Decided 2026-10-31:** English everywhere (UI, reports, code, docs) |
