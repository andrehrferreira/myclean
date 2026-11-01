# 01 Method and accounting

## Sources (all read-only)

| When (local, 2026-11-01) | What | Notes |
|---|---|---|
| 08:16 | `myclean map E:\ --min-gb 0.25 --depth 9 --json` (given) | Not elevated. Scanned 791.3 GiB, not visible 4.0, explained 221.9 (Steam library and Recycle Bin), **unexplained 569.4**, 53 unreadable folders (`Config.Msi`, `Recovery`, `System Volume Information`, `WindowsApps`, the system bin, and 48 pytest and scratch folders under `UzPainter` whose ACL denies listing). |
| 08:30–09:30 | targeted reads | Python and PowerShell at below-normal priority, never more than two folder walks at a time: child sizes and newest writes of named folders (`eage.py`); one walk of E: without the Steam library and the bin (`ewalk.py`: build folders sized but not entered, project markers, files of at least 256 MiB, totals per extension); Steam `appmanifest_*.acf`; the Recycle Bin's `$I` records (`rb.py`, `rb2.py`); `git log`, `git check-ignore`, `git worktree list`, `git branch --merged` with `--no-optional-locks`; `Get-AppxPackage`; `fsutil hardlink list` on two files. |
| 09:00–10:00 | rule emulation (`eproj.py`) | For every build folder of at least 20 MiB beside a marker of a shipped (or proposed) project rule: the rule's targets, `git check-ignore`, the last commit of its work tree and the newest file outside build folders, compared with the rule's inactive days. |
| 09:30–10:30 | duplicates (`dup2.py`) and compressibility (`cmp.py`) | Files of at least 32 MiB outside build folders, `.git`, `node_modules`, the Steam library and the bin, grouped by size, then BLAKE2 of the whole file. zlib level 6 on the first 32 MiB of sample files. |

The scripts are in the session's scratchpad (`…\scratchpad\eage.py`, `ewalk.py`, `eproj.py`,
`dup2.py`, `cmp.py`, `rb.py`, `rb2.py`, `mapkids.py`). Nothing on E: was deleted, moved,
compressed or changed. No cleaner, prune, handler or installer was run, and nothing was shown
on screen. One early `git status` in UzEngine may have refreshed its index's stat cache (no
content change); every later git command used `--no-optional-locks`.

Sizes are GiB. Folder sizes are the map's allocated bytes (hard links counted once) unless a
row says otherwise; rule-emulation and duplicate figures are file bytes.

## Accounting to 100 %

The volume is 953.9 GiB ("Tools & Games", NTFS, NVMe), **795.3 GiB used**, 158.6 GiB free:

| GiB | Part |
|---:|---|
| 791.3 | seen by the non-elevated map |
| 4.0 | not visible: NTFS metadata (2.3 million files and folders, so the MFT alone is ~2.3), `System Volume Information`, the system Recycle Bin, `Config.Msi`, `WindowsApps` (no package of the owner is installed on E:), and 48 pytest and scratch folders under `UzPainter` that the owner's token cannot list (09-8) |
| **795.3** | used |

The top-level folders add up to the 791.3 GiB:

| GiB | Folder | File |
|---:|---|---|
| 509.8 | `E:\UzmiGames` (UzEngine 252.1, Assets3D 157.4, UzPainter 100.4) | 02, 05 |
| 172.8 | `E:\SteamLibrary` | 03 |
| 49.1 | `E:\$RECYCLE.BIN` | 04 |
| 45.0 | `E:\HiveLLM` (24 projects) | 02, 05 |
| 5.9 | `E:\Elyriu` | 05 |
| 3.7 | `E:\Falcor` | 02 |
| 2.8 | `E:\tmp` | 02 |
| 2.1 | `E:\godot` | 02 |
| 0.3 | `E:\Options` | 05 |
| 0.0 | `WpSystem` (Armoury Crate logs), empty `Program Files`, root files | |

Within them, every folder of at least 256 MiB is named in 02–06, and the folders below that
size are 0.9 GiB of HiveLLM, 0.5 of UzEngine, 0.4 of UzPainter and 0.2 of Assets3D: more than
99 % of the used bytes are attributed.

## Why the map explains only 28 %

The map explained the Steam library and the Recycle Bin and nothing else: 569.4 GiB (72 %) is
"unexplained". Every byte of it is in project folders on a workspace drive, for which the packs
have no areas: attribution paths are absolute patterns, so `E:\UzmiGames\…\.zig-cache` cannot be
named without knowing the project's place. 07 adds a depth-by-depth area for `.zig-cache`
(246 GiB); 09-2 proposes that the map use the workspace scanner instead.

## Numbers that differ by method

- A plain walk of E: (Python `os.scandir`) counts 805.6 GiB, 14 more than the map: it followed
  the junctions `Elyriu\Dashboard\node_modules\client` and `\server` (→ 4 GiB of server data
  counted twice) and counted every pnpm hard link in HiveLLM. `ewalk.py` skips junctions (568.0
  GiB outside Steam and the bin, against the map's 569.4).
- Recycle Bin `$I` records list 694 items and 125 GiB; only 10 still have content (50.2 GiB of
  file bytes, 49.1 allocated). The other records point at content Windows already removed (04).
- Steam's `SizeOnDisk` equals the folder sizes (140.4 and 31.2), so the library holds nothing
  besides the games.
