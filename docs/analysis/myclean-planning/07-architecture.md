# 07 — Architecture

## Findings

### F-040 — Three executables, split by privilege and by session
- **Evidence:** Assigning jobs to elevated processes, real-time kernel ETW
  and system-cache cleanup all need SYSTEM or admin rights (F-020, F-022,
  F-011). The clipboard, HKCU, browser profiles and toasts exist only in
  the user's interactive session (F-027).
- **Impact:**

```
 ┌────────────────────── user session ───────────────────────┐
 │  myclean-tray.exe  (user token, auto-start via HKCU Run)    │
 │   • tray icon, notifications, approval UI, main window      │
 │   • input/clipboard watchdog (F-027)                        │
 │   • per-user scans & cleaning (HKCU, browsers, pkg caches)  │
 │                                                             │
 │  myclean.exe  (CLI, --json)    myclean.exe mcp  (stdio MCP)  │
 └──────────────┬──────────────────────────────┬───────────────┘
                │ named pipe, JSON-RPC 2.0, DACL = SYSTEM + owner
 ┌──────────────▼──────────────── session 0 ───────────────────┐
 │  myclean-svc.exe  (Windows service, LocalSystem)            │
 │   • ETW process/network monitor, 1 s sampler                │
 │   • resource governor: job objects, escalation ladder       │
 │   • scheduler (monthly report, recurring auto cleaning)     │
 │   • system scans & built-in system cleaning rules           │
 │   • SQLite store, snapshots, audit log                      │
 └─────────────────────────────────────────────────────────────┘
```

  The CLI and MCP server work without the service for read-only, per-user
  data. Features that need the service say so when it is not running.
- **Confidence:** high.

### F-041 — Keep decision logic pure; isolate Win32 behind small interfaces
- **Evidence:** Most of the risk lives in decisions (which item is safe, when
  to escalate, what changed since last month), and those can be tested
  without a live machine. The Win32 calls are thin.
- **Impact:** `myclean-core` contains no `<windows.h>`: models, rule
  evaluation, plan building, the `SafeRemove` checks, governor policy
  (snapshot in → actions out), snapshot diff, report rendering and the
  knowledge-base lookup. `myclean-platform` implements interfaces such as
  `IFileSystem`, `IProcessTable`, `IJobController`, `IRegistry`,
  `IClipboardProbe` on Win32. Unit tests drive the core with fakes and
  recorded fixtures. Integration tests (Windows-only, in a temp sandbox) cover
  the adapters.
- **Confidence:** high.

### F-042 — Rules, process classes and the knowledge base are data files
- **Evidence:** Cleaner targets, toolchain names and process descriptions
  change far more often than the engine.
- **Impact:** Built-in rules ship in `data/rules/*.toml`, `data/classes.toml`
  and `data/kb/*.json`, validated by a schema at load time (an invalid rule
  is rejected, never half-applied). Owner overrides live in
  `%ProgramData%\MyClean\` (system) and `%APPDATA%\MyClean\` (per user).
  Example rule:

```toml
[[rule]]
id          = "dev.node.node_modules"
category    = "dev-artifacts"
tier        = "auto"
scope       = "user"                 # runs in tray process, user token
project     = { marker = "package.json" }
targets     = ["node_modules", ".next", ".turbo", ".parcel-cache"]
require     = { git_ignored = true, inactive_days = 30, not_in_use = true }
strategy    = "delete"               # delete | quarantine | native | handler
```
- **Confidence:** high.

### F-043 — The UI technology is the main open choice
- **Evidence:** Options for a native C++ Windows UI: Dear ImGui (immediate
  mode, D3D11), WebView2 hosting an HTML UI, WinUI 3 (C++/WinRT), Qt.
- **Impact:** Recommendation: **Win32 tray + Dear ImGui main window**. It is
  one small dependency, fast to iterate, handles large virtualized tables
  and a disk treemap well, and adds no runtime. The cost is a "tool-like"
  look. WebView2 is the alternative if a polished, CCleaner-like look
  matters more; it adds a JS/HTML toolchain. WinUI 3 and Qt cost the most
  for this scope. Owner decision; see the README.
- **Confidence:** medium (a preference).

## Tech stack

| Concern | Choice |
|---|---|
| Language / compiler | C++20, MSVC (VS 2022+), `/W4 /WX /permissive-` |
| Build | CMake + Ninja, `CMakePresets.json`; dependencies via **vcpkg** manifest |
| Windows helpers | **WIL** (RAII handles, error macros), C++/WinRT for Appx and toasts |
| ETW | **krabsetw** |
| Storage | **SQLite** (WAL mode) at `%ProgramData%\MyClean\myclean.db` |
| Config / rules | **toml++**; JSON via **nlohmann/json** (knowledge base, IPC, MCP) |
| Hashing | **BLAKE3** (duplicates), xxHash3 (fast pre-filter) |
| Logging | **spdlog**, rotating files in `%ProgramData%\MyClean\logs` |
| UI | Win32 `Shell_NotifyIcon` + Dear ImGui (D3D11), pending F-043 |
| HTTP (VirusTotal, optional) | WinHTTP (built into Windows, no extra dependency) |
| Tests | GoogleTest + fixtures; integration tests gated to Windows CI |
| Lint / format | clang-tidy, clang-format |
| CI | GitHub Actions `windows-latest` running the full quality gate |

## Repository layout

```
CMakeLists.txt  CMakePresets.json  vcpkg.json
src/
  core/        # pure logic: model, rules, plans, safety, governor policy, diff, report, kb
  platform/    # Win32 adapters: fs, process, etw, jobs, registry, clipboard, msi, wmi, shell
  svc/         # myclean-svc.exe
  tray/        # myclean-tray.exe
  cli/         # myclean.exe (commands + `mcp` server)
data/
  rules/  classes.toml  kb/
tests/
  unit/  fixtures/  integration/
docs/
```

## Storage schema (first cut)

| Table | Contents |
|---|---|
| `scan`, `scan_item` | scan runs and found items (path, size, rule, tier) |
| `plan`, `plan_item` | proposed actions, proposer (ui / mcp / scheduler), approval state |
| `action_log` | append-only audit of everything executed, with undo data |
| `quarantine` | quarantined items, original path, purge date |
| `proc_start` | process starts (image, command-line hash, parent, signer, time) |
| `proc_minute`, `proc_hour` | per-tree CPU/memory aggregates (30 d / 1 y retention) |
| `governor_event` | ladder steps, reasons, metrics, reversals |
| `input_incident` | Ctrl+C/V incidents (F-027) |
| `snapshot` | monthly state for diffs (startup, extensions, services, ports, top folders) |
| `schedule` | in-app schedules with last/next run (catch-up) |

## Install and lifecycle

- `myclean-svc.exe install` (run once, elevated) registers the service with
  `CreateServiceW` (auto start, restart on failure) and writes the tray's
  HKCU `Run` entry. `uninstall` reverses both. An MSI comes later.
- The service runs as a standard Windows service, which is not an OS
  schedule. All timing inside it (scans, reports) is MyClean's own scheduler
  (F-039).
