# 01 Method and accounting

## Sources (all read-only)

| When (local) | What | Notes |
|---|---|---|
| 00:08 | `myclean scan-all --drives C: --json` | Not elevated. 28,328 findings. Counted: remove 96, compress 122, uninstall-review 69, manual-review 415, dedupe 4.4 GiB. Explained 86.7 % of used bytes. |
| 00:49 | (not by this analysis) an approved cleaning plan, run 1 | Moved 13,751 items, **40.7 GiB**, into `C:\.myclean-quarantine\1`: vcpkg build trees and downloads, Playwright browsers, temp folders and others the scan had listed. They stay on C: until the 7-day purge. |
| 01:00 | `myclean map C:\ --min-gb 0.25 --depth 9 --json` | Scanned 848.9 GiB, not visible 52.3, explained 740.9, unexplained 108.0, unreadable entries 276. |
| 01:05–02:30 | targeted reads | PowerShell listings at below-normal priority: `Get-ChildItem` sizes of named folders, the Uninstall keys, `Get-AppxPackage`, the MSI API (`WindowsInstaller.Installer`), the DriverStore, `Win32_PageFileUsage`, `Win32_OperatingSystem`, the WSL rootfs map, `wsl -l -q`. |

Nothing was deleted, moved, compressed or changed. No cleaning, prune or handler was run, and
nothing was shown on screen. The only extra whole-tree walk was the rootfs map. Thresholds
above 200 MB come from the map's 256 MiB floor (16).

## Accounting to 100 %

The volume is 952.5 GiB with 901.2 GiB used at 01:00:

| GiB | Part |
|---:|---|
| 848.9 | seen by the non-elevated scanner (the folders and files of the tree) |
| 52.3 | not visible: used bytes minus scanned bytes (04, "Not visible") |
| **901.2** | used |

Within the 848.9 GiB, the top-level folders add up to 848.2 GiB:

| GiB | Folder |
|---:|---|
| 445.8 | `C:\Users` (the profile 411.0, Public 34.9) |
| 119.1 | `C:\pagefile.sys` |
| 64.1 | `C:\Program Files` |
| 50.9 | `C:\hiberfil.sys` |
| 49.8 | `C:\Windows` (what a non-elevated walk sees, hard links counted once) |
| 40.7 | `C:\.myclean-quarantine` |
| 35.9 | `C:\ProgramData` |
| 35.6 | `C:\Program Files (x86)` |
| 3.2 | `C:\Autodesk` |
| 3.1 | `C:\vcpkg` |
| 0.7 | everything else at the root |

The files 02–11 break each of these down, so more than 99 % of the used bytes are attributed.
The rest is folders under 256 MiB.

## Reconciling "86.7 % explained" with "52 GiB not visible"

The two numbers measure different things:

- `scan-all` (00:08): used 971.3 GB (904.6 GiB). Explained 842.6 GB (784.7 GiB). So 120 GiB was
  unexplained. Of that, 51.8 GiB (55.6 GB) was not visible to the scan. The other ~68 GiB was
  visible but matched no area of the packs.
- `map` (01:00): of the 848.9 GiB scanned, 108.0 GiB matched no area. 40.7 of it is the new
  quarantine folder. 46.9 is folders of at least 256 MiB that no area named (listed by the map).
  The other ~20 is small folders.

So the "unexplained" share is visible-but-unnamed plus not visible, and the unnamed part was
the largest. This task adds areas for all but a few of the 46.9 GiB of named folders (13): the
quarantine, WSL `/tmp`, the NVIDIA, Adobe, Kaspersky, Corsair, Epic, Reallusion, ZBrush,
Marvelous Designer, Gaea, ArtEngine, Nexa, Cortex, Trivy and ort folders, electron-updater caches,
Cursor and Unreal games' logs. What is still unnamed is under 0.5 GiB per folder.

## Two oddities the scan reported

- `placement` said "no healthy drive keeps 10 % free after taking it" for `.rustup` (11 GiB),
  and the cold-data check said "no healthy HDD keeps 10 % free" for 18 GiB. But D: (SSD,
  991 GB free of 3.7 TB), W: (1.76 TB free), K: and L: (HDD, 5.1 and 5.9 TB free) all have room.
  This looks like a target-selection bug (15).
- The page file was 115 GiB at 00:08 and 119 GiB at 01:00. It is system-managed and grows with
  the commit charge (02).
