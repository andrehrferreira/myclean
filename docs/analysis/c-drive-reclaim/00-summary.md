# C: drive reclaim: summary

**Answer.** C: (952 GiB, 901 GiB used, 51 GiB free) can get back about **180 GiB without asking
anyone**, about **200 GiB more with reversible moves** (page file, WSL distribution, toolchains,
hibernation, compression), and up to about **250 GiB more if the owner decides** (sessions,
QuickSaves, content libraries, apps to uninstall, downloads). Two items dominate: a **94 GiB WSL
`/tmp`** full of year-old test leftovers, and a **119 GiB page file** that has to move to another
drive rather than shrink, because the machine commits ~206 GB on 128 GB of RAM.

Measured read-only on 2026-11-01. The `scan-all` run was at 00:08. Someone ran a cleaning plan at
00:49 that moved 40.7 GiB into `C:\.myclean-quarantine`. A `map` of the whole drive followed at
01:00. All sizes are GiB from that map unless marked otherwise. Paths are relative to the
profile (`~`).

## Where the 901 GiB go, and what to do

The status column has three values: **(1)** MyClean deletes it on its own (tier `auto`, 100 %
regenerable), **(2)** reversible move, reduction or compression, **(3)** only the owner decides.
"–" means it is needed as it is.

| GiB | What | Action | Status | Reclaim GiB |
|---:|---|---|:-:|---:|
| 119.1 | `C:\pagefile.sys`, system-managed (commit 206 of 224 GB) | system-managed page file on D: or W:, a small one (or a dedicated dump file) on C:; restart | 2 | ~110 |
| 125.2 | WSL 1 Ubuntu-24.04 rootfs | — | | |
| ↳ 93.9 | `/tmp`: 39,000 `.tmpXXXXXX` test folders, 98 % from Sep–Dec 2025 | `wsl-docker/tmp-prune` (new): `find /tmp … -mtime +7 -exec rm -rf` run inside Linux | 1 | ~93 |
| ↳ 31.3 | `/usr` 18.7 (CUDA 4.9, Ollama 3.3, Swift 2.5), `/home` 9.0, `/opt` 2.6 | move the distribution to D: (`wsl-docker/distro-files`, after the prune) | 2 | 31.3 |
| 99.7 | Program Files 64.1 + (x86) 35.6 | uninstall candidates (05); compress (x86) with LZX | 3 / 2 | ~30 / 18.2 |
| 50.9 | `C:\hiberfil.sys` (full) | reduced hibernation (keeps Fast Startup) / off | 2 / 3 | 25.4 / +25.4 |
| 49.8 | `C:\Windows` | DISM, driver packages and update leftovers (ask); restart to install the staged update | 3 | ~2–8 |
| 46.7 | `~\AppData\Local\UzPainter` | pytest-temp 9.5 (new auto); cache 1.3 (ask); work stores 36 (owner) | 1 / 3 | 9.5 / 36 |
| 40.7 | `C:\.myclean-quarantine\1` (tonight's run, 13,751 items) | purged on its own after 7 days (by 2026-11-08), or now from the tray | 1 | 40.7 |
| 35.9 | `C:\ProgramData` | VS installer payloads 10.1 and NVIDIA driver packages 3.7 (new auto); the rest is app data (06) | 1 / – | 13.8 |
| 34.8 | `C:\Users\Public\Documents` | ZBrush QuickSave 17.6, Reallusion templates 15.0, Marvelous Designer 1.5 | 3 | 34.1 |
| 23.4 | `~\.codex` | sessions 18.5, thread history 2.4, images 1.3 | 3 | ~22 |
| 21.9 | Chrome profile | caches are auto already; Service Worker 7.6, the on-device model 4.0 and site data are the owner's | 3 | ~13 |
| 14.3 | `%TEMP%` | `windows/temp-user` (auto, older than 2 days, not in use); `swap.vhdx` 6.5 belongs to the running WSL 2 VM | 1 | ~6.5 |
| 12.9 | VS Code data | webview resource cache 10.5 (new, ask); VSIX cache 1.1 (now auto) | 3 / 1 | 10.5 / 1.1 |
| 11.4 | `~\.rustup` | move with RUSTUP_HOME, **or** LZX compression (~5), not both | 2 | 11.4 |
| 11.4 | `~\.elyriu-local-ai` | 5.9 of `.incomplete` downloads (June); models, runtime | 3 | ~6–11 |
| 10.9 | Android SDK 8.2 + `~\.android` 2.8 | SDK Manager / Device Manager | 3 | ~7 |
| 20.4 | Documents 9.2, Downloads 8.8 (a Windows 11 ISO alone is 8.1), Desktop 2.4 | owner | 3 | ≤20 |
| 7.2 | `~\.gradle` | unused versions (now auto, 90 days); move GRADLE_USER_HOME | 1 / 2 | ? / 7 |
| 6.8 | `~\.claude` | transcripts; lower `cleanupPeriodDays` | 3 | ≤6.6 |
| 6.9 | Store Python 3.12 | pip --user packages 6.0 | 3 | ≤6 |
| ~30 | other dev caches, updaters, game logs, apps (07–11) | new and re-tiered auto cases (13), app data | 1 / 3 | ~15 |
| 52.3 | not visible without admin | WindowsApps ~15, shadow storage ~25–30 (estimate), NTFS metadata ~4–5, protected Windows folders | 3 | ? |

## Totals

| | GiB on this machine |
|---|---:|
| **(1) MyClean deletes on its own** (auto; quarantine purge 40.7, WSL `/tmp` ~93, VS payloads 10.1, pytest-temp 9.5, `%TEMP%` ~6.5, NVIDIA driver packages 3.7, updater leftovers ~3.5, package-manager prunes ~4, Electron/Bun/Trivy ~4, shader caches and game logs ~4.6, PowerToys 1.5, VSIX 1.1, others) | **~185** |
| **(2) reversible: move, reduce or compress** (page file ~110, WSL distribution 31.3, hibernation reduced 25.4, Program Files (x86) LZX 18.2, `.rustup` 11.4, `.gradle` ~7) | **~200** |
| **(3) owner decision** (UzPainter work 36, ZBrush/Reallusion/MD 34, Codex 22, uninstalls ~30, hibernation off +25, Chrome site data and model ~13, Documents/Downloads/Desktop ≤20, VS Code webview cache 10.5, local AI 6–11, Android ~7, Claude 6.6, Python 6, …) | **~250** |

The three totals don't overlap: an item counted under (1) is not counted again, and a folder
that can be either moved or compressed is counted once. The quarantine figure is space that
tonight's run already freed elsewhere on C:. It is held for a restore and comes back by itself.

## What changed in the knowledge packs

- 15 new clean cases. Twelve are `auto`: VS installer payloads, NVIDIA app downloads, electron-updater
  leftovers, PowerToys installers, Electron and Bun caches, Trivy's Java DB, UzPainter's pytest-temp,
  Reallusion downloads, Unreal games' logs and crash reports, their shader byte code, and the WSL
  `/tmp` prune. Three are `ask`: the VS Code webview cache, the Autodesk installers and UzPainter's
  cache.
- 28 rules re-tiered to `auto`. This needed changes to the cleaning spec and its tests (13).
- 30 new or changed attribution areas.
- A WSL rootfs is now excluded from compression (code, 03).
- All packs validate, and the packs, clean, cleanup, compress and rules tests pass (13).

Owner decisions with exact actions: 14. Follow-ups for code: 15.
