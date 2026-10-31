# 02 — Disk cleaning and relocation

How MyClean frees space on C: and keeps it free. There are three tools,
cheapest first: **relocate** what does not need to be on C:, **clean** what
can be regenerated, and **report** what needs a human decision.

## Findings

### F-011 — Windows already ships safe cleanup handlers; use them
- **Evidence:** Disk Cleanup (`cleanmgr`) and Storage Sense are front-ends for
  COM handlers that implement `IEmptyVolumeCache`/`IEmptyVolumeCache2`. They
  are registered under
  `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\VolumeCaches`
  (Windows Update Cleanup, Delivery Optimization Files, Temporary Setup Files,
  Windows Error Reporting, and so on).
- **Impact:** For system areas (F-004), MyClean calls these handlers
  (`Initialize` → `GetSpaceUsed` → `Purge`) instead of deleting files itself.
  That handles locked files, servicing state and the rules Windows applies to
  its own folders. Component-store cleanup is done with
  `DISM /Online /Cleanup-Image /StartComponentCleanup`.
- **Confidence:** high for the mechanism. Phase 2 opens with a spike to
  confirm which handlers can run without UI from a service.

### F-012 — Developer artifacts can be found by marker files and confirmed with git
- **Evidence:** Each toolchain marks a project root with a known file and
  writes regenerable output to known folders. In a repository those folders
  are almost always in `.gitignore`.
- **Impact:** The project scanner walks the configured workspace roots (F:,
  W:, …), finds project roots, and proposes artifact folders. A folder qualifies
  only if **all** of the following hold:
  1. it matches the rule for that project type (table below);
  2. inside a git repo, `git check-ignore` reports it as ignored, so
     source-controlled files are never touched;
  3. no process has its current directory or open files inside it (no build
     is running there);
  4. the project has been inactive for at least N days (default 14, set per
     rule). Activity is the newest of: last commit, source-file mtime.
- **Confidence:** high.

| Project marker | Artifact folders (default) | Native command, when better |
|---|---|---|
| `package.json` | `node_modules`, `.next`, `.nuxt`, `.turbo`, `.parcel-cache`, `.svelte-kit`, `dist`, `build`, `coverage`, `.cache` | — |
| `Cargo.toml` | `target` | `cargo clean` |
| `*.sln`, `*.csproj`, `*.vcxproj` | `bin`, `obj`, `.vs`, `x64`, `Debug`, `Release` (when ignored) | — |
| `CMakeLists.txt` | `build*`, `out`, `cmake-build-*` | — |
| `build.gradle(.kts)` | `build`, `.gradle` | — |
| `pom.xml` | `target` | — |
| `pyproject.toml`, `setup.py`, `requirements*.txt` | `__pycache__`, `.pytest_cache`, `.mypy_cache`, `.ruff_cache`, `.tox`, `*.egg-info`, `build`, `dist` (`.venv` is opt-in) | — |
| `go.mod` | — (caches are global) | `go clean -cache` |
| `pubspec.yaml` | `.dart_tool`, `build` | `flutter clean` |
| `*.uproject` | `Intermediate`, `DerivedDataCache`, `Binaries`, `Saved/Logs`, `Saved/Crashes` (never all of `Saved`: it holds config) | — |
| Unity (`Assets/` + `ProjectSettings/`) | `Library`, `Temp`, `Obj`, `Logs` | — |

### F-013 — Global caches should be pruned with each tool's own command
- **Evidence:** Package managers keep indexes and content-addressed stores
  that break if files are deleted underneath them (pnpm's store, Cargo's
  registry index).
- **Impact:** Each global-cache rule picks a strategy: run the native prune
  command when one exists (`pnpm store prune`, `npm cache verify`,
  `yarn cache clean`, `pip cache purge`, `uv cache prune`,
  `dotnet nuget locals all --clear`, `go clean -cache -modcache`,
  `docker system prune`), otherwise delete version folders older than N days
  (Gradle `caches/<version>` and `wrapper/dists`, JetBrains old IDE folders,
  VS `ComponentModelCache`).
- **Confidence:** high.

### F-014 — Relocation removes the cause; cleaning only treats the symptom
- **Evidence:** F-003 and F-005: TEMP and the package caches grow back on C:
  because their location settings point there.
- **Impact:** A guided **Relocate** feature. For each item it shows the current
  location, the size and a proposed target volume. When the owner applies a
  move, MyClean:
  - sets the user environment variable or tool config (`TEMP`/`TMP`,
    `GRADLE_USER_HOME`, `CARGO_HOME` plus a `PATH` fix for `.cargo\bin`,
    `npm config set cache`, `NUGET_PACKAGES`, `PIP_CACHE_DIR`,
    `GOMODCACHE`/`GOCACHE`, Unreal DDC `UE-LocalDataCachePath`);
  - moves the existing content, or lets it regenerate;
  - stores the previous value so **Undo** restores it exactly.

  Directory junctions are avoided when a supported setting exists, because
  junctions confuse some tools and make deletion more dangerous (F-031).
- **Confidence:** high for the env-var and config moves. Each tool's behavior
  after a move is checked in phase 2.

### F-015 — Hibernation, page file and restore points are settings, not files
- **Evidence:** F-002.
- **Impact:** They live in an **Optimizations** screen. Each entry shows its
  current value, the space it costs, a recommended value, the command it will
  run (`powercfg /h /type reduced`, page-file move via WMI
  `Win32_PageFileSetting`, `vssadmin resize shadowstorage`) and an undo.
  Nothing here is ever automatic.
- **Confidence:** high.

### F-016 — The rest of the catalog: well-known regenerable data
- **Evidence:** Standard locations on Windows 11.
- **Impact:** The initial rule set (data files, see
  [07-architecture.md](07-architecture.md)):
  - **Windows:** user `%TEMP%` (files older than 2 days and not locked),
    `C:\Windows\Temp`, `%LOCALAPPDATA%\CrashDumps`, `ProgramData\Microsoft\Windows\WER`,
    `C:\Windows\LiveKernelReports`, thumbnail and icon caches, Recycle Bin
    per volume, `Windows.old` (through its handler).
  - **GPU shader caches:** `%LOCALAPPDATA%\NVIDIA\DXCache`, `GLCache`,
    `D3DSCache`, AMD `DxCache`. They regenerate, at the cost of one-time
    stutter in games, so they default to "ask".
  - **Browsers** (Chrome, Edge, Brave, Firefox, every profile): HTTP cache,
    code cache and GPU cache only. Never cookies, history, passwords or
    sessions unless a rule explicitly opts in.
  - **Electron apps** (Discord, VS Code, Slack, Teams…): `Cache`,
    `Code Cache`, `GPUCache`, `Crashpad`.
  - **IDEs:** VS Code `CachedData` and old `workspaceStorage` entries,
    JetBrains caches and old version folders, Visual Studio
    `ComponentModelCache`.
  - **Game launchers:** Steam `appcache/httpcache`, `depotcache`, leftover
    `downloading`/`temp` folders; Epic `webcache`.
  - **WSL and Docker:** report the size of each `.vhdx`. Compacting runs
    `wsl --shutdown` followed by `Optimize-VHD` or `diskpart compact vdisk`.
    It is always "ask", because it stops running distros and containers.
- **Confidence:** high for the locations. Default ages and tiers get tuned
  after the phase-1 scan.

### F-017 — Fast scanning needs a better primitive than recursive `FindFirstFile`
- **Evidence:** C: alone holds about 870 GB of data. Recursive enumeration of
  the user profile in PowerShell timed out after 300 s during this analysis.
- **Impact:** The scanner runs in two stages:
  1. **v1:** `FindFirstFileExW` with `FindExInfoBasic` and
     `FIND_FIRST_EX_LARGE_FETCH` (or `NtQueryDirectoryFile` with
     `FileIdBothDirectoryInformation` and large buffers), parallel per
     directory, without following reparse points. Good enough for targeted
     roots.
  2. **v2 (elevated):** read the NTFS `$MFT` directly for whole-volume
     treemaps, the way WizTree does, and keep the index fresh with the USN
     change journal (`FSCTL_READ_USN_JOURNAL`). The journal also feeds "what
     grew since last month" for the monthly report.
- **Confidence:** medium. v2 is substantial work and needs a spike to
  measure the gain.

### F-018 — `Windows\Installer` orphans can only be moved, never deleted
- **Evidence:** F-004, 10.9 GB. Windows Installer looks up cached `.msi`/`.msp`
  files by path when uninstalling or repairing.
- **Impact:** MyClean lists every file referenced by an installed product or
  patch (`MsiEnumProductsEx`, `MsiGetProductInfoEx(INSTALLPROPERTY_LOCALPACKAGE)`,
  `MsiEnumPatchesEx`) and treats the rest as orphan candidates. It
  **moves** them to quarantine on another volume (F-030) and never deletes
  them directly. This is an "ask" item with a clear warning.
- **Confidence:** medium. Orphan detection has known edge cases, which is why
  only moving is allowed.

## Automatic recurring cleaning

The owner wants recurring cleanup without being asked every time. Each rule
has a **tier**:

| Tier | Meaning | Default examples |
|---|---|---|
| `auto` | Runs on schedule with no prompt. Only regenerable data, only after preconditions pass | `%TEMP%` older than 2 days, crash dumps, Electron and browser HTTP caches, artifacts of projects inactive for 30+ days |
| `ask` | Scanned on schedule; the results wait in the tray for approval | shader caches, global package caches, `Windows\Installer` orphans, old Downloads, VHDX compaction |
| `report` | Shown only; MyClean never acts on it | personal files, duplicates, large files, installed apps |

The owner can promote or demote any rule. `auto` runs are logged and can be
undone where possible: quarantined items can be restored, but a purged
cache only regenerates.
