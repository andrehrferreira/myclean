# 04 Windows internals (49.8 GiB visible + most of the 52.3 GiB not visible)

The PC has not restarted since **2026-09-30** (last boot). Several items below wait for a restart.

| GiB | Folder | What it is | Reclaim | Status |
|---:|---|---|---|:-:|
| 11.3 | `System32` (DriverStore 6.1 of it) | Windows | — | – |
| 6.1 | `System32\DriverStore\FileRepository` | Current drivers: NVIDIA `nv_dispi` 2.66, `u0405277` 1.07, AMD `amdocl` 0.96 (2 versions), Alienware `awcc` 0.50, Intel Wi-Fi `netwtw08` 0.25 (5 versions), Hello face 0.15 (2) | handler `Device Driver Packages` (ask) removes only the older versions: **< 1 GiB** here | 3 |
| 11.0 | `Installer` | Windows Installer cache: 516 .msi/.msp (7.2 GiB), plus `Razer Central` 2.5, `$PatchCache$` 0.7, `Razer` 0.5 | **Never delete** (repairs, patches and uninstalls need it). The MSI API lists 505 packages as referenced by installed products and patches. **11 .msp files, 1.33 GiB, are referenced by nothing** (the largest is 0.72 GiB from 2026-06-08). Moving them off is possible but not counted | 3 |
| 9.1 | `WinSxS` (hard links counted once) | Component store. `DISM /AnalyzeComponentStore` needs admin and was not run | `windows/component-store` (DISM StartComponentCleanup, ask). Typically frees 1–4 GiB after a month of updates. `WinSxS\Backup` is 0.46 | 3 |
| 7.3 | `SoftwareDistribution\Download` | **A staged cumulative update**: `Package_for_RollupFix 26100.9711`, SSU 26100.9703, downloaded 2026-10-13 (6.3 GiB in 34,764 files) plus 0.8 GiB of loose payloads. No reboot flag is set yet | **Restart** (Settings > Windows Update), and Windows installs it and removes the download. Deleting it is not safe, and the %WINDIR% deny-list forbids it anyway | 3 |
| 6.1 | `assembly` | .NET Framework native images (GAC) | — | – |
| 2.4 | `Panther` (the map counted 0.7 on disk) | Setup and upgrade logs | handler `Windows Upgrade Log Files` (windows-setup-logs, ask) | 3 |
| 1.4 | `SysWOW64` | Windows | — | – |
| 0.9 | `SystemApps` | Windows | — | – |
| 0.8 | `Microsoft.NET` | .NET Framework | — | – |
| 0.4 | `Fonts` | fonts | — | – |
| 0.21 | `C:\Windows.old` | Previous installation (remnant) | handler `Previous Installations` (ask) | 3 |
| 0 | `$WINDOWS.~BT`, `$WINDOWS.~WS`, `ESD`, `MEMORY.DMP`, `Minidump` | absent or empty | — | – |
| 0.06 | `Logs\CBS` | servicing logs | — | – |
| ? | `Temp`, `SystemTemp`, `Prefetch`, `LiveKernelReports`, `WUModels`, `Logs`, the font cache, the Delivery Optimization cache, the search index (`ProgramData\Microsoft\Search\Data`) | **access denied without admin** | `windows/temp-system` (auto, older than 2 days); `delivery-optimization`, `wer-system`, `minidumps` handlers. `Get-DeliveryOptimizationStatus` shows no active jobs | 1 / 3 |

## Handlers now `auto`, and those that stay `ask`

Coordinator decision, cleaning spec "Handlers of regenerable data are auto". These handlers
remove only caches and setup leftovers, so they are now `auto`:

- Thumbnail Cache
- Internet Cache Files
- Delivery Optimization Files
- Temporary Setup Files
- Upgrade Discarded Files

These stay `ask`, because they remove something the owner could still need:

- Update Cleanup (the ability to uninstall updates)
- Device Driver Packages (driver rollback)
- Previous Installations (going back to the previous Windows)
- Windows ESD installation files (needed for Reset this PC)
- error reports and memory dumps (crash evidence)
- reset and setup logs
- Language Pack
- Recycle Bin (the owner's deleted files)

## Not visible: 52.3 GiB

This is what the volume uses minus what a non-elevated walk can read. It is made of:

| Est. GiB | What | How it was sized |
|---:|---|---|
| ~15 | `C:\Program Files\WindowsApps` (Store apps: OpenAI Codex 2.3, Photos 0.9, Armoury Crate 0.65, AMD Radeon Software 0.65, WinDbg 0.64, …) | summed through `Get-AppxPackage` install locations: 14.96 GiB |
| ~4–5 | NTFS metadata ($MFT for ~4.1 million entries at 1 KiB each, $LogFile, the USN journal, $Secure) | arithmetic |
| ~2–5 | protected Windows folders (`Temp`, `Prefetch`, `Logs`, `LiveKernelReports`, `WUModels`, the search index, the Delivery Optimization cache, `C:\Recovery`, `Config.Msi`), Defender and `ProgramData\Microsoft\Windows` protected folders | 276 unreadable entries listed by the map |
| **~25–30** | **`System Volume Information`**: restore points and shadow copies | the remainder. `Win32_ShadowStorage` refuses non-elevated callers |

To check (owner, elevated, read-only): `vssadmin list shadowstorage` and
`vssadmin list shadows /for=C:`. Capping the shadow storage (setting `shadow_storage`, for
example `vssadmin resize shadowstorage /for=C: /on=C: /maxsize=10GB`) removes the oldest restore
points that no longer fit. That is (3).

## Protected, never cleaned

`C:\Windows\Installer`, `C:\ProgramData\Package Cache` (2.76 GiB: copies needed to repair and
uninstall), `C:\Windows\assembly` and `C:\Recovery` stay as they are.
