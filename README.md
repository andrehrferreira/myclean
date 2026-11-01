# 🧹 MyClean

**⚡ Windows 11 system monitor, cleaner and optimizer for heavy developer and game-dev workstations**

![C++](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Platform](https://img.shields.io/badge/platform-Windows%2011-0078D6.svg)
![Toolchain](https://img.shields.io/badge/MSVC-CMake%20%2B%20Ninja%20%2B%20vcpkg-orange.svg)
![License](https://img.shields.io/badge/license-MIT-green.svg)
![Status](https://img.shields.io/badge/status-pre--release-yellow.svg)

[What is MyClean?](#-what-is-myclean) • [Features](#-key-features) • [Architecture](#%EF%B8%8F-architecture) • [Quick Start](#-quick-start) • [Documentation](#-documentation) • [Roadmap](#%EF%B8%8F-roadmap) • [Contributing](#-contributing)

---

## 🎯 What is MyClean?

MyClean is a **resident system manager** for Windows 11. A background
service watches **every CPU core, the memory, every SSD and HDD and every
process**, explains what it sees, and keeps the machine in shape. It does the
work automatically where that is safe, asks for one-click approval where it
is not, and **measures and can undo every change** it makes.

**Think CCleaner + Process Lasso + CrystalDiskInfo + WizTree in one native
service**, built for machines where the hardware is rarely the bottleneck.
On those machines the real problems are:

- **The system drive is always full** while other drives sit half empty.
  Temp files, package caches, build outputs, the hibernation file and the
  page file all land on `C:`.
- **Builds and test runs take every core and all the RAM** and freeze the
  desktop. Games have the same load profile and must never be throttled.
- **Drives fail or fill up without warning**, because nothing watches their
  health, wear, errors or free-space trend.
- **Small freezes**, such as a stall on Ctrl+C / Ctrl+V, leave no trace in
  any log.

> **Status: pre-release.** Monitoring (M1), freeing C: (M2) and the
> machine-wide scan are code complete and run on the reference machine; their
> acceptance runs are still open. The resource governor (M3) is in progress.
> There is no installer before v1.0.

## ✨ Key Features

### 📊 Monitoring
- **1 s telemetry**:
  - CPU per core, with throttling and DPC time;
  - memory: commit, standby, compression, hard faults;
  - **every physical disk**: throughput, IOPS, latency, queue, % idle;
  - volumes, GPU, network per process;
  - process trees.
- **Two years of history**: 1 s raw for 1 h, then per-minute for 30 days and
  per-hour for 2 years, in SQLite.
- **Days-until-full projection** for every volume.

### 💽 Disk Health
- **NVMe health log**: temperature, % used, available spare, media errors,
  data written. **SATA SMART**: reallocated, pending and uncorrectable
  sectors, CRC errors.
- **Change alerts.** A new media error or reallocated sector raises an alert
  even below any threshold.
- **Endurance projection**: TBW used vs rated, and years left at the current
  write rate.

### 🗂️ Space & Cleaning
- **Space analysis on any drive**:
  - top files and folders;
  - a live NTFS index (`$MFT` + USN journal) that shows what grew in
    24 h / 7 d / 30 d;
  - duplicates, old downloads, installers you already used.
- **Data-driven cleaning rules** for Windows, browsers, Electron apps, IDEs,
  GPU shader caches, game launchers and package managers. Windows system
  areas are cleaned through **Windows' own cleanup handlers**.
- **Developer artifacts** (`node_modules`, `target`, `bin/obj`, `build`,
  Unreal `Intermediate`, Unity `Library`…). A folder is removed only if git
  ignores it, the project is inactive, and nothing is using it.
- **Three tiers**:
  - `auto`: regenerable data, on a schedule;
  - `ask`: quarantined and restorable;
  - `report`: shown only, never acted on.

### 🧭 Placement
- **Relocation** of what keeps regrowing (TEMP, Gradle, Cargo, npm, NuGet,
  pip, Go and Unreal DDC caches) off the system drive, through each tool's
  supported setting, with exact undo.
- **Placement advisor**: which workload belongs on which drive (NVMe, QLC,
  USB, HDD), based on health and free space. Also a Dev Drive guide.
- **Hibernation, page file and restore-point storage**: shown with their
  cost, changed only on approval.

### 🛡️ Resource Governor
- **Build and test trees are capped at launch.** Job objects with a CPU hard
  cap and a whole-tree memory cap are applied the moment `cl`, `rustc`,
  `cargo`, `msbuild`, `gradle`, `jest`, `pytest`… start.
- **Reversible escalation ladder** for any other runaway:
  priority → EcoQoS → cap → suspend. Kill is opt-in only.
- **Game detection and game mode.** Games are never touched. On X3D CPUs,
  builds can be moved to the non-V-Cache CCD while you play.
- **Fail-open.** Stopping the service lifts every cap. After a crash, the
  restarted service re-adopts its jobs.

### ⚙️ Optimizer
- **Startup manager** with measured boot impact; disable/enable with undo.
- **Drive maintenance** that runs only when idle: SSD re-TRIM, HDD defrag.
- **Indexer scope** (exclude build folders), **leak detector**,
  **boot/logon tracking**, and a **background-app cost** report.
- **Verify loop.** Every optimization is measured before and after, and
  undone automatically if it made things worse.

### ⌨️ Input Watchdog
- **Copy/paste stall detection** that adds no hook of its own.
- **Attribution.** Each stall is tied to a mechanism (delayed rendering,
  clipboard held open, viewer chain, slow keyboard hook, heavy payload) and
  to a process.

### 🔍 Guard, Apps & Backups
- **Intruder scan**: persistence points, browser extensions with their
  install origin, and background network talkers. Verdicts come only from
  **Microsoft Defender** or an **opt-in, hash-only VirusTotal** lookup.
- **Unused apps.** Last-used comes from several sources and always shows its
  source and confidence. Removal goes through each app's own uninstaller.
- **Backup check**: Windows backups, third-party tools, backup drives, and
  git repos with unpushed work.

### 🤖 AI Integration (MCP)
- **`myclean mcp`**: a stdio MCP server with read tools and proposal tools
  for Claude Code and other MCP clients.
- **Propose, never execute.** An AI can create a plan; only a click in the
  tray runs it.

### 📅 Reports
- **Monthly report** of what changed: startup items, extensions, ports,
  unsigned binaries, top folders, disk health, governor actions, freezes and
  space freed. It is scheduled inside the app and catches up if the PC was
  off.

## 🔐 Safety Principles

- **Nothing destructive happens outside a plan.** Every change has a
  preview, a verification step and an undo, and is recorded in an
  append-only audit log.
- **Every deletion goes through one function** (`SafeRemove`). It enforces a
  hard deny-list, never follows junctions or symlinks, never downloads
  cloud-only files, and re-checks each item right before acting.
- **The privileged service accepts no raw paths or commands** over IPC.
  Per-user cleanup runs with the user's own token.
- **No kernel driver.** No data leaves the machine unless you opt in.
- **No registry "cleaning", no RAM "boosters", no generic tweak packs.**

## 🏗️ Architecture

```
┌──────────────────────────────── user session ─────────────────────────────────┐
│  myclean-tray.exe                         myclean.exe                         │
│  tray · dashboard (ImGui + ImPlot)        CLI (--json)                        │
│  approval inbox · input watchdog          `mcp` stdio server                  │
│  per-user cleaning (user token)                                               │
├──────────────────────── named pipe · JSON-RPC 2.0 · ACL ──────────────────────┤
│  myclean-svc.exe  (Windows service, LocalSystem)                              │
│  collectors (PDH · NtQuerySystemInformation · ETW) · disk health (NVMe/SMART)  │
│  analyzers → advisor → policy engine → executor → verify                       │
│  governor (job objects, ladder, game mode) · scheduler (catch-up)             │
├───────────────────────────────────────────────────────────────────────────────┤
│  SQLite (WAL): time series · inventories · plans · append-only audit log     │
└───────────────────────────────────────────────────────────────────────────────┘
```

Decision logic lives in a pure C++ core (`src/core`, no `<windows.h>`) and is
tested by replaying recorded fixtures. Win32 sits behind small interfaces in
`src/platform`. Full design: [docs/architecture.md](docs/architecture.md).

## 🚀 Quick Start


### Requirements
- **Windows 11** (x64)
- **Visual Studio 2022** or its Build Tools, with *Desktop development with C++*
- **CMake 3.28+**, **Ninja**, **PowerShell 7**

### Build, test and deploy locally

There is no installer before v1.0. One script builds, runs the test gate and
hot-deploys the service, the tray app and the CLI on the local machine:

```powershell
pwsh -File tools\deploy\deploy.ps1            # build + test; deploy only if green and changed
pwsh -File tools\deploy\deploy.ps1 -Watch     # redeploy after every good change
pwsh -File tools\deploy\deploy.ps1 -Status    # active version, service, tray, history
pwsh -File tools\deploy\deploy.ps1 -Rollback  # back to the previous version
pwsh -File tools\deploy\deploy.ps1 -Uninstall
```

Builds are installed side by side under `C:\Program Files\MyClean\versions\`,
and a `current` junction points at the active one. A failed health check
rolls back automatically. See [docs/deploy.md](docs/deploy.md).

### Look at your drives

```powershell
myclean map C:                  # where C:'s space went, and how to get it back
myclean scan-all                # one report for every fixed drive
myclean scan-all --drives C:,D: --budget-minutes 5 --json
myclean packs list              # loaded knowledge packs
myclean packs validate <path>   # run a pack's fixtures
```

These commands only read. Changes are made through plans that you approve in
the tray.

### Use from Claude Code (MCP)

```json
{
  "mcpServers": {
    "myclean": { "command": "C:\\Program Files\\MyClean\\current\\bin\\myclean.exe", "args": ["mcp"] }
  }
}
```

## 🎯 Use Cases

### 💾 "My C: drive is always full"
Find what grew, clean regenerable data on a schedule, and move TEMP and
package caches to a drive with space so they stop coming back.

### 🔥 "A build froze my PC"
Compilers and test runners start inside a capped job. The desktop stays
responsive and the build finishes, just slower.

### 🎮 "I'm gaming while a build runs"
Game mode protects the game, tightens the build cap and, on X3D CPUs, keeps
the build off the V-Cache cores.

### 🩺 "Is my SSD dying?"
Health history and change alerts for every NVMe and SATA drive, with an
endurance projection.

### 🤖 "What is slowing this computer down?"
Ask Claude Code. MyClean's MCP tools answer with real data and can propose
fixes that you approve in the tray.

## 🛠️ Technology Stack

| Concern | Choice |
|---|---|
| Language / toolchain | C++20, MSVC (`/W4 /WX /permissive-`), CMake + Ninja, vcpkg |
| Windows | WIL, C++/WinRT, PDH, ETW via [krabsetw](https://github.com/microsoft/krabsetw), job objects, storage IOCTLs |
| Storage | SQLite (WAL) |
| Config / data | toml++, nlohmann/json |
| Hashing | BLAKE3, xxHash3 |
| UI | Win32 tray + Dear ImGui + ImPlot (D3D11) |
| Logging | spdlog |
| Tests / quality | GoogleTest, fixture replay, clang-tidy, clang-format, GitHub Actions |

## 📚 Documentation

### Product
- [Product Requirements](docs/prd.md): goals, requirements, non-goals, prompt acceptance
- [Architecture](docs/architecture.md): processes, modules, collectors, storage, action framework, security

### Planning
- [Roadmap](docs/roadmap.md): milestones M0–M6 and exit criteria
- [Work DAG](docs/dag.md): work packages, dependencies, critical path
- [Initial analysis](docs/analysis/myclean-planning/README.md): measurements and findings F-001 … F-043

### Development
- [Development guide](docs/development.md): layout, modules, conventions, quality gate
- [Knowledge packs](docs/packs.md): how to add cleaning, relocation and compression cases as data
- [Spikes](docs/spikes/): X1–X8 findings from real hardware
- [Local deployment](docs/deploy.md): `deploy.ps1` and the binary contracts
- [Rulebook tasks](.rulebook/tasks/): one task per milestone, with specs

## 🗺️ Roadmap

| Milestone | Delivers | Status |
|---|---|---|
| **M0 Foundation** | Build, CI gate, safety core, storage, action framework, scheduler | ✅ Done |
| **M1 See everything** | Service, tray, dashboard, telemetry, disk health, input watchdog, CLI, MCP read tools | 🧪 Code complete, acceptance open |
| **M2 Free C:** | Space analysis, cleaning tiers, quarantine, relocation, approval inbox | 🧪 Code complete, acceptance open |
| **M2b Machine-wide scan** | NTFS index, `map`, `scan-all`, drive maintenance, compression, knowledge packs | 🧪 Code complete, acceptance open |
| **M3 Never freeze** | Resource governor with game protection | 🔄 In progress |
| **M4 Optimize** | Startup, indexer scope, leak detector, verify loop (NTFS index and drive maintenance came with M2b) | 📋 Planned |
| **M5 Guard** | Intruder scan, unused apps, backup check | 📋 Planned |
| **M6 v1.0** | Monthly report, full acceptance suite | 📋 Planned |

Details: [docs/roadmap.md](docs/roadmap.md) and [docs/dag.md](docs/dag.md).

## 🛠️ Development

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

Quality gate on every commit: type-check (`/W4 /WX`), then clang-tidy, then
the tests covering the change. The full suite must pass before a push.

For major features, this repo uses
[@hivehub/rulebook](https://github.com/hivellm/rulebook) for spec-driven
development. Proposals, tasks and specs live under
[`.rulebook/tasks/`](.rulebook/tasks/).

## 🤝 Contributing

1. Fork the repository and create a feature branch.
2. For anything beyond a small fix, open or pick a Rulebook task first.
3. Keep the quality gate green: `/W4 /WX`, clang-tidy and tests.
4. Open a pull request describing the change and how it was verified.

## 📄 License

MIT. See [`LICENSE`](LICENSE).

## 🙏 Acknowledgments

- [CCleaner](https://www.ccleaner.com/), [Process Lasso](https://bitsum.com/),
  [CrystalDiskInfo](https://crystalmark.info/) and
  [WizTree](https://diskanalyzer.com/): the tools that inspired this.
- [krabsetw](https://github.com/microsoft/krabsetw), [WIL](https://github.com/microsoft/wil),
  [Dear ImGui](https://github.com/ocornut/imgui), [ImPlot](https://github.com/epezent/implot),
  [SQLite](https://sqlite.org/): the foundations it is built on.

## 📞 Contact

- 🐛 Issues: [github.com/andrehrferreira/myclean/issues](https://github.com/andrehrferreira/myclean/issues)
- 💬 Discussions: [github.com/andrehrferreira/myclean/discussions](https://github.com/andrehrferreira/myclean/discussions)

---

<div align="center">

**Built with ❤️ in C++** ⚙️

_Monitor everything, clean safely, never freeze_

[⭐ Star on GitHub](https://github.com/andrehrferreira/myclean) • [📖 Docs](docs/) • [🚀 Quick Start](#-quick-start)

</div>
