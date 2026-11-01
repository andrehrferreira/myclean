# 07 Owner decisions

Ordered by size. Each one names exactly what goes, what stays, and how to undo or check first.
None of it is done by MyClean on its own.

| # | GiB | Decision | Reversible? |
|---|---:|---|---|
| 1 | 111.99 | Drop `repo-recuperado`'s working tree, keep its `.git` | yes: `git checkout` recreates it |
| 2 | 68.67 | Delete `001.bundle` (byte-identical pack in the clone) | only through the clone |
| 3 | ~650 | Move the backups and the download archives to an HDD | yes (cold-data move) |
| 4 | 75.61 | Delete archives already extracted on G: | no (re-download) |
| 5 | 32.03 + 11.68 | Approve Unreal output (ask) | yes: rebuilt |
| 6 | 26.96 | Megascans library: drop what projects already hold | re-download via Fab |
| 7 | 16.17 | Delete the second copy of two Leartes archives | yes: the first copy stays |
| 8 | (23.66) | Trim `_Backups\NetworkSample` to its sources (the same bytes as its share of 5, 11 and the VS caches of 06) | output only |
| 9 | 13.45 | Unreal autosaves | no |
| 10 | 10.29 | Kitbash3D Unity variants in the 2024 backup | re-download |
| 11 | 6.49 | Project `Binaries` and packaged `Build` folders outside git, MutableSample streamed data | rebuilt by building, packaging, compiling |

## 1–2. The 2023 git copies (180.7 GiB of 249.5)

`G:\ToS\_Backups\Backup - ToS3\2023` holds the same repository three times (02): `001.bundle`
(68.67), the clone's `.git` (68.69, the identical pack) and the clone's working tree (111.99).

To keep the full history, including the 1,449 keep-around and 1,255 merge-request commits that
only the bundle names, in 68.7 GiB:

```
cd "G:\ToS\_Backups\Backup - ToS3\2023\repo-recuperado"
git fetch ..\001.bundle "refs/keep-around/*:refs/keep-around/*" "refs/merge-requests/*:refs/merge-requests/*"
git bundle verify ..\001.bundle          # optional: reads 69 GiB
git fsck --connectivity-only             # all refs reachable
```

The fetch writes no new objects (they are all in the pack), only refs. Then:

- **2:** delete `001.bundle` (68.67 GiB). Keep `001.refs` and `LATEST` (233 KB) as the record.
- **1:** make the clone bare: move `repo-recuperado\.git` to `repo-recuperado.git`, set
  `git config core.bare true` in it, delete the rest of `repo-recuperado` (111.99 GiB, a checkout
  of `master` at 2021-11-27 with 19 checkout-side differences in files whose names have
  parentheses or commas). `git worktree add` or `git clone` brings a working tree back.
- The empty repository `2023\.git` (no commit) can go with it.

## 3. Move the cold backups and archives to an HDD (~650 GiB, reversible)

Nothing on G: except `G:\Substance\previews` was created or written after 2026-04-17, and
`_Backups` nothing after 2025-11-15. scan-all's cold-data report (6 months, at least 1 GiB) will
propose the outermost cold folders, which on G: are whole top-level trees (`G:\ToS` 1,032 GiB as
one folder). Moving live project trees to an HDD makes the editor slow, so the useful moves are:

| GiB | Folder |
|---:|---|
| 216.03 + 134.98 + 68.69 | `G:\ToS\_Backups` after 1, 2 and 8 (its archives and the bare 2023 repository included) |
| 230.50 | the download archives outside `_Backups` that are the only copy: `ToS\_Base\Bases ZBrush` 59.62, `ToS\Assets - 3th\Leartes Studios` 47.42, `NoSafePlaces\Assets\LevelDesign` 42.20, `Volodymyr Stepaniuk` 27.98, `Characters` 20.19, `Weapons` 9.19, `Bundles` 6.90, others 16.99 |

Per the C: analysis K: (5.1 TB free) and L: (5.9 TB free) are healthy HDDs. G: is at 17.9 %
free, so this is optional; it would take G: to ~53 % free.

## 4. Archives whose content is already extracted (75.61 GiB)

Measured by name and size of each archive's members against the files on G: (04). The archive
is the pristine download; the extracted copy may have been changed since. Keep the archive if
it is the only way to get the original again.

| GiB | Archives | Extracted copy |
|---:|---|---|
| 19.51 | 16 OBJ/FBX zips in `ToS\_Base\Bases ZBrush\*` (DEMON parts 1–4, WINGS, CRAWLERS, TORSOS, LEGS, TAILS, Horns Volume 2, JAWS, iNSEKt) | next to them, same folder |
| 14.27 | `ToS\Assets - 3th\ErrantPhoton\ErrantLandscapeStampLibrary.zip`, `ErrantWorlds-BasicLandscapeMaterial.zip` | **only in the 2025 backup** (`Client\Content\ErrantWorlds`) |
| 11.83 | rars and zips in `_Backups\…\2024\Assets` (ArtStation Zahra/Alma, Polyphoria, AdvancedTraversalSystem, CGTrader Dremorn) and a Google Drive zip in `Design` | inside the same backup |
| 9.24 | `ToS\_Base\Brushes` (`ZBrush Advanced Brushes pack - ROCKS.rar` 4.70, `Rock PBR Textures.rar` 3.30, `Woods.zip`, `EM-Ornaments-Brush-PNG.rar`) | next to them |
| 9.08 | `NoSafePlaces\Assets\Characters\Modular_Military_Character_UE_5.0_-_5.6.part1–5.rar` | `NoSafePlaces\Client\Content` |
| 4.51 | `NoSafePlaces\AssetsNormalization\Content\AutoRockCliffs.rar`, `PCGGeometryDecals.rar` | next to them |
| 4.24 | `NoSafePlaces\Assets\LevelDesign\Outpost and Bunker Environment UE 5.0 - 5.1.zip` | `NoSafePlaces\Client\Content` |
| 2.94 | `Marcus Whinney\Dragon HornTail` rar, `Weapons\Gun_Weapon_Grenade…rar`, `Animations\65_Zombie_Idle…rar`, `Murloc-UE5.6-V2.zip` | live trees |

## 5. Unreal output (ask)

- In git today, offered by `unreal/project-artifacts`: `2025\Client` 6.77, `2024\AssetsNormalize`
  4.91 (both backups).
- After 06-E (code change): 32.03 GiB in projects outside git, `ToS\Client\Intermediate` 10.31
  the largest. Approving rebuilds the C++ modules on the next editor start (minutes) and the
  derived data on first use.

## 6. The Megascans library (26.96 GiB)

Every file of 1 MiB or more in `G:\UE\Megascans Library` is also in a project (04). If no new
project will take assets from it, delete `Downloaded\UAssets\<id>` folders the projects hold, or
the whole library, and re-download from Fab when needed.

## 7. Two archives stored twice (16.17 GiB)

`G:\NoSafePlaces\Assets\LevelDesign\FireWatchTower.zip` (11.68) and `HauntedPrison_5.0.zip`
(4.49) are byte-identical (sampled hash, same size) to the files in
`G:\ToS\Assets - 3th\Leartes Studios`. Keep one copy.

## 8. `_Backups\NetworkSample` (25.78 GiB, 23.66 of it output)

Not in git, so no rule offers it until 06-E. Its `.vs` (17.29), `Intermediate` (3.92),
`Binaries` (0.53), `Saved\Cooked`/`StagedBuilds`/`Shaders`/`Logs`/`Crashes` (1.42) and `Build`
(0.50, a packaged game) are output. The project itself (`Source`, `Config`, `Plugins` 2.06,
`Content` 0.03, `NetworkSample.uproject`) is ~2.1 GiB. The same goes for `UE - Projects\PlayerController`
(`.vs` 3.31 and `Intermediate` 2.41 of 41.99).

## 9. Unreal autosaves (13.45 GiB)

`Saved\Autosaves` keeps the editor's autosaved copies of packages for crash recovery: 9.25 GiB
in `_Backups\Backup - ToS3\2025\Client`, 2.63 in the creature projects (`GiantWorm` 1.56), 0.63
in the 2024 backup, `ClothTest55` 0.53 (with `Saved\Backup`), `NoSafePlaces\Client` 0.37. Once
the project was saved and closed they are stale, but only the owner knows.

## 10. Kitbash3D Unity variants (10.29 GiB)

`_Backups\Backup - ToS3\2024\Assets\Kitbash3D\kb3d_medievalsiege.unity.hdrp.4k.zip` (7.71) and
`kb3d_medievalsiege.unity.hdrp.zip` (2.57) sit next to `kb3d_medievalsiege.unreal.4k.zip`; the
project moved from Unity (2023) to Unreal. Kitbash3D purchases can be downloaded again from the
account.

## 11. Binaries, packaged builds and Mutable data (6.49 GiB)

Project `Binaries` outside git (1.48: `ClothTest55` 0.59, `NetworkSample` 0.53, ~0.06 in each
other project) are rebuilt from `Source` only when the project has one.
`UE_Projects\ClothTest55\Build\Windows` (1.71) is the same packaged game as its
`Saved\StagedBuilds` (identical files; that one is in 5), and `_Backups\NetworkSample\Build`
(0.50) likewise. `UE_Projects\MutableSample\Saved\MutableStreamedDataEditor`
2.80: Mutable's compiled streaming data for the editor; probably recompiled from the
Customizable Objects, not verified here.

## Smaller items

- Duplicates inside the 2024 backup: `tos_HUD_2024_ingame (1).psd` (1.91, identical to the file
  without "(1)"), two spellings of the `Dragon Temple` rar (0.60).
- `Assets - 3th\Marcus Whinney`: the same tutorial video in 5 folders (0.42 GiB of copies).
- `_Backups\Server - LastCSharp` (3.03): same commit as the live `ToS-Server-5` plus 5 uncommitted
  changes; 1.80 of it is `node_modules`, `target`, `.vs` and `bin` (MyClean cleans those). Commit or
  copy the 5 changes, then the backup is redundant.
- Not visible (3.17): `System Volume Information` may hold shadow copies of G:; an elevated
  `vssadmin list shadowstorage /for=G:` shows it.
