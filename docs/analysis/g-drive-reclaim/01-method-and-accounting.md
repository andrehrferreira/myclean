# 01 Method and accounting

## Sources (all read-only)

| When (local) | What | Notes |
|---|---|---|
| 08:12 | `myclean map G:\ --min-gb 0.25 --depth 9 --json` | Scanned 1,526.2 GiB, not visible 3.17 GiB, **explained 336 KB** (the Recycle Bin), unexplained 1,526.2 GiB in 894 named folders, 3 unreadable entries (`Config.Msi`, `System Volume Information`, the SYSTEM Recycle Bin). The binary that ran it rejected the `wsl-docker` pack (`'wsl tmp prune' is not a native command MyClean runs`): it predates that change. No effect on G:. |
| 08:39 | Python `os.scandir` walk of G: (no link following) | One row per folder: direct bytes, file count, newest write. 136,173 folders, 557,825 files, 1,527.03 GiB logical. 31 s. |
| 08:42 | the same walk, every file of at least 1 MiB | 63,736 files. Files under 1 MiB add up to 26.8 GiB. |
| 08:45 | marker walk | Every `*.uproject`, `*.sln`, `*.slnx`, `*.csproj`, `*.vcxproj`, `CMakeLists.txt`, `package.json`, `Cargo.toml`, `pyproject.toml`, `setup.py`, `requirements*.txt`, `ProjectVersion.txt` and `.git` entry. |
| 08:46–09:00 | git, read-only | `log -1`, `rev-parse`, `rev-list --count`, `show-ref`, `remote -v`, `bundle list-heads`, `check-ignore`, `ls-files`, `--no-optional-locks status` (so the index is not rewritten). |
| 08:48 | duplicate check | Files of at least 64 MiB with the same byte size, confirmed by SHA-1 of three 1 MiB samples (start, middle, end). |
| 08:50 | archive listing | Every `.zip`, `.rar` and `.7z` of at least 256 MiB: zip central directory (Python `zipfile`), rar/7z headers (`7z l -slt`, stdin closed). Nothing was extracted. |
| 08:56 | newest creation or write time | Per folder down to depth 3, for the cold-data view. |

Every PowerShell and Python process ran at below-normal priority. Nothing was deleted, moved,
compressed or written outside `docs/analysis/g-drive-reclaim/` and the session scratchpad. No
window was shown. At most two heavy readers ran at once (the 8-minute `git status` of the
recovered repository overlapped the archive listing).

**Sizes.** The tables use the walk's logical sizes (GiB = 2^30 bytes). They differ from the
map's on-disk sizes by 0.05 % overall. The one visible difference is `G:\ToS\Server\target`:
28.32 GiB logical, 26.83 GiB on disk, because Cargo hard-links its outputs from `deps` and the
map counts a hard-linked file once. Deleting it frees the map's figure.

## Accounting to 100 %

The volume is 1,863.0 GiB with 1,529.38 GiB used and 333.61 GiB free (17.9 %):

| GiB | Part |
|---:|---|
| 1,526.21 | seen by the scanner (on disk) |
| 3.17 | not visible: NTFS metadata (an MFT for ~700,000 entries is ~0.7 GiB), `System Volume Information` (shadow storage, if System Protection is on for G:; needs admin to size), `Config.Msi` |
| **1,529.38** | used |

The top-level folders (map, on disk):

| GiB | Folder | Newest change (creation or write) |
|---:|---|---|
| 1,032.49 | `G:\ToS` | 2026-04-17 |
| ↳ 676.72 | `_Backups` | 2025-11-15 |
| ↳ 181.56 | `_Base` (ZBrush bases, brushes, meshes) | 2025-07-08 |
| ↳ 80.97 | `Assets - 3th` (purchased packs, mostly archives) | 2025-07-24 |
| ↳ 48.14 | `Assets - Custom` (the owner's creatures, props) | 2026-04-17 |
| ↳ 27.72 | `Server` (Rust; `target` 26.83) | 2025-11-30 |
| ↳ 14.27 | `Client` (Unreal project) | 2026-04-08 |
| ↳ 3.11 | `Tools`, `Plugins`, `Design`, `Cutscenes`, `Concept`, `Refs`, `Substance` | 2025-12-01 |
| 354.17 | `G:\NoSafePlaces` (Unreal project 183.76, asset archives 146.25, normalization project 24.15) | 2025-08-29 |
| 74.02 | `G:\UE_Projects` (six Unreal projects) | 2025-08-25 |
| 32.31 | `G:\UnderTheShadow` (Unreal project) | 2026-02-04 |
| 26.96 | `G:\UE\Megascans Library` | 2026-03-07 |
| 5.92 | `G:\GenericServer` | 2025-09-07 |
| 0.27 | `G:\SteamSDk` | 2024-07-21 |
| 0.06 | `G:\Substance` (previews; written 2026-10-29) | 2026-10-29 |
| 0.0003 | `G:\$RECYCLE.BIN` | 2025-04-11 |

Files 02–05 break every one of these down. The ledger below gives each byte exactly one line:
every file of at least 1 MiB is an entry, and each folder's smaller files are one more. A file
named by an explicit list (the bundle, an archive) takes that line; otherwise the most specific
folder rule wins.

| Status | GiB (logical) | Lines |
|---|---:|---|
| (1) auto, existing rules + one new in-spec rule | 41.73 | `rust/target` 28.32 + 0.52, new `visual-studio/intellisense-cache` 11.07, `node/artifacts` 1.46, `dotnet` 0.37 |
| (1b) auto after a small code change | 33.77 | the same Visual Studio caches in projects outside git |
| (3) owner decides (incl. ask-tier rules) | 363.27 | see 00 and 07 |
| (2) reversible move to an HDD | 650.20 | cold backups and download archives |
| – keep | 438.07 | project content, ZBrush bases, the owner's assets |
| **total** | **1,527.03** | = the walk |

## Why the map explained 0 % of G:

Attribution areas name fixed places (`%LOCALAPPDATA%\...`, `?:\pagefile.sys`, ...). G: holds
only project trees, backups and asset archives at owner-chosen paths, so no area matches.
Developer artifacts are found by the project rules (`project = { marker = ... }`), which
`myclean map` does not use. 115 GiB of G: are artifact folders those rules know (03). Code
follow-up in 08.
