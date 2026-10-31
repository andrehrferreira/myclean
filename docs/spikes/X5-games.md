# X5 — Game detection signals

- **Work package:** G3 (phase 3, `.rulebook/tasks/phase3_resource-governor`)
- **Finding:** F-023 (`docs/analysis/myclean-planning/03-resource-governor.md`)
- **Spec:** `.rulebook/tasks/phase3_resource-governor/specs/governor/spec.md`,
  requirement **Game Detection** (also **Game Mode**, **Protected Processes
  Receive No Action**, **Governor Overhead and Effect**)
- **Machine:** Windows 11 Pro build 26200, pt-BR locale, Ryzen 9 7950X3D,
  RTX 4090 + AMD iGPU, 5120×1440 primary monitor. Run 2026-10-31, non-elevated.
- **Scratch code:** outside the repo (`x5.cpp`, `luid.cpp`, `pdhcost.cpp`), built
  with MSVC 2022 x64. The key parts are quoted below.

## Questions

1. What does `HKCU\System\GameConfigStore` really contain, and which values
   give an exe path or a title?
2. How do we build "library roots" from Steam, Epic, GOG and Xbox/WindowsApps,
   and which drives hold games?
3. What does `SHQueryUserNotificationState` return in normal desktop use, and
   with a fullscreen app?
4. What do PDH `\GPU Engine(*)\Utilization Percentage` instance names look like,
   how do we sum them per process, and what does it cost?

## 1. GameConfigStore

### Method

```powershell
$k='HKCU:\System\GameConfigStore'
Get-ItemProperty $k                                  # top-level values
$c = Get-ChildItem "$k\Children"; $p = Get-ChildItem "$k\Parents"
$c | % { $_.GetValueNames() } | Group-Object | Sort Count -Desc   # value histogram
# 'Parent' is a DPAPI blob; decrypt as the current user:
Add-Type -AssemblyName System.Security
[Text.Encoding]::Unicode.GetString(
  [Security.Cryptography.ProtectedData]::Unprotect($c[0].GetValue('Parent'),$null,'CurrentUser')).TrimEnd([char]0)
# Parents key name = SHA-1 of the lower-case exe name in UTF-16LE:
$sha=[Security.Cryptography.SHA1]::Create()
(($sha.ComputeHash([Text.Encoding]::Unicode.GetBytes('h1z1.exe'))) | % { $_.ToString('x2') }) -join ''
```

### Observed

Layout:

| Key / value | Kind | Meaning (observed) |
|---|---|---|
| `GameConfigStore` (root) | DWORD/binary | Game Bar settings only (`GameDVR_Enabled`, `GameDVR_FSEBehaviorMode`, `Win32_AutoGameModeDefaultProfile` …). No per-game data. |
| `Children\{GUID}` | key | One per registered executable. **110** keys on this machine. |
| `…\Type` | DWORD | `1` = Win32 exe (95 entries), `2` = packaged app with an AUMID name (15 entries). |
| `…\Flags` | DWORD | Seen: 17 (0x11), 19, 21 (0x15, all Type 2), 51 (0x33), 561 (0x231). Bit meanings are not documented, so we don't rely on them. |
| `…\Parent` | binary (≈262 B) | **DPAPI blob** (`01 00 00 00 D0 8C 9D DF 01 15 D1 11 …`). Decrypts as CurrentUser to the **lower-case exe file name** in UTF-16 (for example `h1z1.exe`). |
| `…\MatchedExeFullPath` | REG_SZ | Full exe path, original case. Present on **43/110** entries, and all of them also have `LastAccessed`. |
| `…\LastAccessed` | QWORD | FILETIME of the last run (43 entries). Newest: 2026-09-20. |
| `…\ExeParentDirectory` | REG_SZ | Only the name of the parent folder (for example `common`, `Win64`, `Palworld`), not a path. 24 entries. |
| `…\TitleId`, `…\GameDVR_GameGUID` | REG_SZ | Xbox title id (87 entries) and a GUID (95 entries). |
| `…\ProcessDependencies`, `…\WorkingDirectory` | — | Rare (9 and 4 entries). |
| `Parents\<40 hex>` | key | **106** keys. Name = `SHA1(UTF-16LE(lowercase(exeName)))`, verified for `h1z1.exe` → `86ac5f66…9f05`. |
| `Parents\…\Children` | REG_MULTI_SZ | GUIDs of the matching `Children` keys (1 to 3 per name, 110 in total). |

Entry groups:

- **Path-matched** (43 entries: Type 1, Flags 17/19/51, `MatchedExeFullPath`
  + `LastAccessed`): games that actually ran here. Drives: **E: 39**, C: 3,
  G: 1. Examples: `E:\SteamLibrary\steamapps\common\Palworld\Pal\Binaries\Win64\Palworld-Win64-Shipping.exe`,
  `E:\Albion\game\Albion-Online.exe`, `E:\Jogos\Diablo IV\Diablo IV.exe`,
  `C:\Program Files (x86)\Grinding Gear Games\Path of Exile 2\PathOfExile.exe`.
  Also non-games that Game Bar flagged: a benchmark (`C:\Users\<user>\AppData\Roaming\UserBenchmark\…\CUBE.exe`)
  and an emulator on the Desktop.
- **Name-only** (52 entries: Type 1, Flags 51/561, no path, no
  `LastAccessed`): titles that never ran here, matched by file name alone. Several
  names are generic: `fm.exe`, `acs.exe`, `grid.exe`, `ffx.exe`, `aces.exe`,
  `ed8.exe`, `shootergame.exe` (the default name of the Unreal sample project),
  `cube.exe`.
- **Packaged, Type 2** (15 entries, Flags 21): **not games.** They include
  `PythonSoftwareFoundation.Python.3.8/3.10/3.12…!python`, `…!pip`,
  `Microsoft.DesktopAppInstaller…!winget`, `Microsoft.MSPaint`, the Ubuntu WSL
  distros, the NVIDIA Control Panel, AMD Radeon Software, Armoury Crate and
  Remote Desktop.
- About 30 path-matched entries point at games that are no longer installed
  (`E:\SteamLibrary\steamapps\common` now has 4 folders). That is harmless,
  because a path match only fires when that exe runs.

### Conclusion

GameConfigStore is a useful signal, but only its **Type 1 path-matched
entries** are trustworthy. Read `MatchedExeFullPath` (plain text) and compare it
with the process image path, case-insensitively. Type 2 entries must be
**ignored**: treating them as games would protect `python.exe` and `winget`,
which are the `test`/`build` workloads the governor exists to cap. A name-only
match through `Parents` (SHA-1 of the file name, no DPAPI needed) is a weak
signal because of generic names. Use it at most to raise confidence together
with the GPU signal.

The service runs as LocalSystem and must read the hive as
`HKU\<owner SID>\System\GameConfigStore`. `MatchedExeFullPath` and the
`Parents` hash need no DPAPI. Decrypting `Parent` would need the owner's
context (the tray), and nothing requires it. *Not tested from SYSTEM, because
this run was non-elevated.*

## 2. Library roots

### Method

```powershell
(Get-ItemProperty 'HKCU:\Software\Valve\Steam').SteamPath          # c:/program files (x86)/steam
Get-Content "$steam\steamapps\libraryfolders.vdf"                   # KeyValues text
Get-ChildItem "$lib\steamapps" -Filter 'appmanifest_*.acf'          # per-app installdir
Get-ChildItem 'C:\ProgramData\Epic\EpicGamesLauncher\Data\Manifests' -Filter *.item |
  % { Get-Content $_.FullName -Raw | ConvertFrom-Json } |
  Select DisplayName, InstallLocation, LaunchExecutable, AppCategories, bIsApplication
Test-Path 'HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games'
Get-AppxPackage | ? { Test-Path (Join-Path $_.InstallLocation 'MicrosoftGame.config') }
foreach ($d in (Get-Volume | ? DriveLetter).DriveLetter) { Test-Path "${d}:\.GamingRoot"; Test-Path "${d}:\XboxGames" }
Get-PhysicalDisk | Select FriendlyName, BusType, MediaType
```

### Observed

- **Steam:** `libraryfolders.vdf` (Valve KeyValues text, nested `"N" { "path" … "apps" { "<appid>" "<bytes>" } }`)
  lists **5 libraries**: `C:\Program Files (x86)\Steam` (only Steamworks
  Shared), `E:\SteamLibrary` (3 manifests, 4 folders in `common`), and
  **`W:\SteamLibrary`, `L:\SteamLibrary`, `F:\SteamLibrary`, which do not
  exist** (no `steamapps`). Entries are stale. `L:` is the "Backup 2" HDD.
  Each `appmanifest_<id>.acf` has `installdir` (relative to `steamapps\common`),
  `name`, `LastPlayed` and `SizeOnDisk`. It also has `LastOwner`, a SteamID,
  which is personal data and must not be stored.
- **Epic:** 24 `.item` JSON manifests and **zero games**. All are
  `Unreal Engine` 5.2–5.8, plus Quixel Bridge, Fab, MetaHuman plugins and
  "ARK DevKit Beta" (`ShooterGameEditor.exe`), installed under `W:\UE_5.x`.
  `AppCategories` is `engines/ue5,engines` (or empty for plugins), and 14 of the
  24 point at a folder that no longer exists. `LauncherInstalled.dat` (JSON)
  lists the same engine/plugin installs. The launcher itself lives in
  `W:\Epic Games\Launcher`.
- **GOG:** not installed (`HKLM\SOFTWARE\WOW6432Node\GOG.com\Games` absent).
- **Xbox / Microsoft Store:** no `XboxGames` folder and no `.GamingRoot` on any
  drive. No package has `MicrosoftGame.config`, and the GamingServices registry
  key is absent. `C:\Program Files\WindowsApps` and **`E:\WindowsApps`** (Store
  apps were moved to E:) cannot be listed non-elevated
  (`UnauthorizedAccessException`). `C:\Program Files\WindowsApps` contains
  **`PythonSoftwareFoundation.Python.3.12`** and `Microsoft.DesktopAppInstaller`
  (winget).
- **Where the games are:** almost all on **E: "Tools & Games"**, a
  `ROG ESD-S1C` 954 GB SSD on **BusType USB**. `Get-Volume` reports it as
  `DriveType Fixed`, so a drive-type check cannot tell that it is USB. Games
  live in `E:\SteamLibrary` and also in **non-library folders**: `E:\Jogos\…`,
  `E:\Albion`, `E:\Arena Breakout Infinite`, `E:\Netmarble Game`,
  `E:\Project.Zomboid.v41.78.16`. Of the 43 path-matched GameConfigStore entries,
  29 are under `steamapps\common` and **14 are outside any launcher root**.
  There is no `E:\Games` folder. One more title is under `G:\UE\…`.

### Conclusion

- Steam roots = the `path` entries in `libraryfolders.vdf` **that exist**, plus
  `\steamapps\common`. Re-read the file when it changes, and skip missing drives
  quietly (the backup drive L: is listed).
- **Epic manifests must be filtered.** Install locations whose
  `AppCategories` contain `engines`, or that are plugins with no
  `LaunchExecutable`, are **not** game roots. Otherwise `UnrealEditor.exe` and
  `ShaderCompileWorker.exe` under `W:\UE_5.x` become protected "games", while
  F-019 puts `UnrealBuildTool`/`ShaderCompileWorker` in class `build`. Only
  manifests with `AppCategories` containing `games` (none here, so the exact
  value is **not verified**) and an existing `InstallLocation` should count.
- **`WindowsApps` must not be a game root.** It holds Store Python and winget,
  and it is not even listable. Use `XboxGames` folders, which the Xbox app
  creates and which are listable, and the `MicrosoftGame.config` marker in
  the package's install folder. Neither exists here, so the Xbox path is
  **not verified on this machine**.
- Owner-defined roots are essential here (`E:\Jogos`, `E:\Albion` …), and
  GameConfigStore path matches cover most of the rest.

## 3. SHQueryUserNotificationState

### Method

```cpp
QUERY_USER_NOTIFICATION_STATE s{};
HRESULT hr = SHQueryUserNotificationState(&s);   // shell32, called from the user session
HWND fg = GetForegroundWindow(); DWORD fgPid; GetWindowThreadProcessId(fg, &fgPid);
```

### Observed (normal desktop use)

Every query from the interactive session (session 1), with ordinary desktop
apps in the foreground, returned `hr=S_OK`, **`QUNS_ACCEPTS_NOTIFICATIONS` (5)**.
That includes a run where the test window existed but did **not** get the
foreground.

### Fullscreen case: **not verified — needs a manual test by the owner**

A test window (borderless 5120×1440 `WS_POPUP`, then DXGI
`SetFullscreenState(TRUE)` on a flip-model swap chain) was run twice before the
owner asked us to stop; it took over the owner's screen and must not be
repeated unattended. Preliminary readings from those runs, to be confirmed
manually:

| Test window state | Foreground? | Result |
|---|---|---|
| windowed 640×480, rendering | yes | 5 `ACCEPTS_NOTIFICATIONS` |
| borderless fullscreen, rendering | yes | 2 `QUNS_BUSY` |
| DXGI exclusive (`GetFullscreenState` = TRUE) | yes | 3 `QUNS_RUNNING_D3D_FULL_SCREEN` |
| borderless fullscreen, idle (no rendering) | yes | 2 `QUNS_BUSY` |
| any state | **no** (foreground lock kept another app in front) | 5, and `SetFullscreenState` failed with `0x887A0022` `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` |

Suggested manual test for the owner: in a terminal run
`Start-Sleep 20; x5.exe quns` (the read-only probe), then switch to a game
within 20 s. Do this once with a borderless-windowed game and once with a game
in exclusive fullscreen.

### Conclusion (provisional until the manual test)

- The value appears to describe the **foreground** window only. A fullscreen
  window that is not foreground reported 5.
- Borderless fullscreen, which is the common mode for modern games, appears to
  report `QUNS_BUSY`, not `QUNS_RUNNING_D3D_FULL_SCREEN`. An idle borderless
  window (like a fullscreen video or slideshow) also reports `QUNS_BUSY`. So the
  spec's "and sustained GPU use" condition is necessary, not optional.
- The API is per user session. It must be called in the **tray** (session 1)
  and passed to the service; a LocalSystem service in session 0 has no user
  desktop. *Not tested from session 0.*

## 4. PDH GPU Engine counters

### Method

```cpp
PdhOpenQueryW(nullptr, 0, &q);
PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &c); // English name, works on pt-BR
PdhCollectQueryData(q); Sleep(1000); PdhCollectQueryData(q);
PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items);
// per item: swscanf_s(name, L"pid_%lu_", &pid); engtype = wcsstr(name, L"_engtype_") + 9;
```

Adapters were mapped with `IDXGIFactory1::EnumAdapters1` → `DXGI_ADAPTER_DESC1::AdapterLuid`.

### Observed

- Instance name format:
  `pid_<PID>_luid_0x<HIGH8>_0x<LOW8>_phys_<n>_eng_<n>_engtype_<TYPE>`,
  for example `pid_100932_luid_0x00000000_0x0001DEDF_phys_0_eng_12_engtype_3D`.
- **887–920 instances** on an idle desktop, across 3 LUIDs:
  `0x02135831` = RTX 4090 (480 instances), `0x0001DF7B` = AMD Radeon iGPU (44),
  `0x0001DEDF` = Microsoft Basic Render Driver (396; it exposes `eng_0…eng_31`,
  all `engtype_3D`, for every GPU process).
- `engtype` values seen: `3D` (432), `Copy`, `VideoDecode`, `VideoEncode`,
  `Video Decode 1`, `Video Codec 0`, `Video JPEG`, `Compute 0`, `Compute 1`,
  `High Priority 3D`, `High Priority Compute`, `LegacyOverlay`, `OFA_0`,
  `Security`, `Security 1`, `Security_1`, `Timer 0`, `VR`. **Type names contain
  spaces and vary by vendor.** A plain `3D` match excludes `High Priority 3D`.
- The wildcard counter picks up **new** processes on each collect (our pid
  appeared after the D3D device was created). The **first** collect that
  contains a new instance yields 0, so a process needs two samples before it
  has a value.
- Test window (`ClearRenderTargetView` ×50 + `Present(1)` at about 120 fps),
  `engtype_3D` summed over all instances of its pid: windowed 8.4 %,
  borderless 14.7–23.6 %, exclusive 16.7 %, **idle borderless 0.0–0.2 %**.
- Cost: one 1 Hz collect + format of about 890 instances takes **1.9 ms on
  average (max 2.3 ms)**. Process CPU over 10 samples was below the 15.6 ms
  timer resolution, so under 0.16 %.
- `Get-Counter '\GPU Engine(*)\Utilization Percentage'` **crashed pwsh 7 (x86)
  with an access violation** inside `PdhHelper.GetCounterInfoPlus`. That doesn't
  affect native code, but tests must not shell out to `Get-Counter`.
- `PdhAddCounterW` with the English path also succeeded on this pt-BR system.
  This counter set does not appear to be localized here, but
  `PdhAddEnglishCounterW` is the safe choice.

### Conclusion

Per-process 3D use = for each pid and LUID, the sum of `engtype_3D` instances,
then the max over LUIDs (so the Basic Render Driver and the iGPU don't dilute
the dGPU). No real game was run in this spike, so the "sustained" threshold
(level and duration) is **not calibrated**. It must be calibrated on 3+ real
titles (tasks.md 1.2), including one on E:. The idle-borderless reading
(≤ 0.2 %) against the trivial render loop (8–24 %) suggests that a low
threshold separates "fullscreen and rendering" from "fullscreen and static".

## Implications for the specs

1. **Game Detection** — "registered in `HKCU\System\GameConfigStore`" should
   say: a Type 1 entry whose `MatchedExeFullPath` equals the image path. Type 2
   (packaged) entries are excluded, because they include Store Python and
   winget. Name-only entries are not enough on their own.
2. **Game Detection** — "Xbox `WindowsApps`" as a library root conflicts with
   **Process Classification** and **Proactive Build and Test Jobs**:
   Store Python (`test`/`build`) lives in `WindowsApps`. Replace it with
   `<drive>:\XboxGames` plus the `MicrosoftGame.config` marker.
3. **Game Detection** — "Epic" roots must exclude Unreal Engine/plugin
   manifests (`AppCategories` containing `engines`). Otherwise UnrealEditor and
   ShaderCompileWorker (class `build`, F-019) become protected.
4. **Game Detection** — Steam roots come from `libraryfolders.vdf` and must be
   checked for existence. Stale entries include the backup drive L:.
5. **Game Detection** — the example root `E:\Games` doesn't exist. The real
   owner roots are `E:\Jogos`, `E:\Albion` and others; 14 of 43 played titles
   are outside launcher roots. Seed the owner's root list from the parent
   folders of path-matched GameConfigStore entries that sit outside known
   roots, as a suggestion the owner confirms.
6. **Game Detection, "Fullscreen with GPU use"** — the scenario says
   "exclusive fullscreen". The preliminary data suggests borderless games
   report `QUNS_BUSY`; keep both states, as the requirement already does, and
   make the scenario cover borderless too. Fullscreen behavior is **not
   verified — needs a manual test by the owner**.
7. **Game Detection** — `SHQueryUserNotificationState` must be read in the tray
   (user session) and sent to the service; GameConfigStore must be read through
   `HKU\<owner SID>`.
8. **Governor Overhead and Effect** — a 1 Hz GPU Engine wildcard sample costs
   about 2 ms wall time and negligible CPU, so it fits NFR-PERF-1. Still,
   sample GPU only while a candidate (fullscreen foreground) exists, since
   there are about 900 instances.
9. Detecting "USB SSD" for E: needs `BusType` (`IOCTL_STORAGE_QUERY_PROPERTY` /
   `MSFT_PhysicalDisk.BusType`); `GetDriveType` reports Fixed.
10. Open item for G3: calibrate the GPU threshold on 3+ real titles, one on E:,
    as tasks.md 1.2 asks. This spike could not run games.
