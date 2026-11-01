# G: drive reclaim: summary

**Answer.** G: ("ToS", 1,863 GiB NVMe, 1,529 GiB used, 334 GiB free) can get back about
**42 GiB without asking anyone** (28.8 of it the Rust `target` the coordinator already counted,
11.1 from one new Visual Studio cache rule), **34 GiB more on its own after one small code
change** (the same caches in projects that are not in git), about **363 GiB if the owner decides**
(mostly redundant copies: a git repository stored three times, archives already extracted, a
duplicated Megascans library, Unreal build output), and **650 GiB by moving cold backups and
download archives to an HDD**. G: is a game-development data drive: Unreal projects, a 676 GiB
backup folder and 457 GiB of asset archives. Nothing on it was written after 2026-04-17 except a
60 MB Substance preview folder.

Measured read-only on 2026-11-01: the `map` at 08:12, then a full walk, a marker walk, read-only
git, sampled hashes and archive listings (01). Sizes are GiB (2^30), logical; on disk they differ
by 0.05 % (`ToS\Server\target` is 26.8 on disk because Cargo hard-links its outputs).

## Where the 1,527 GiB go, and what to do

The status column has three values: **(1)** MyClean deletes it on its own (tier `auto`, 100 %
regenerable), **(2)** reversible move, **(3)** only the owner decides (including ask-tier rules).
**(1b)** is auto once 06-E (`outside_git`) is approved. "–" means it is needed as it is.

| GiB | What | Action | Status | Reclaim GiB |
|---:|---|---|:-:|---:|
| 180.7 | `_Backups\Backup - ToS3\2023`: `001.bundle` 68.67 + the clone's working tree 111.99. The clone's `.git` (68.69, in the move row) holds a pack **byte-identical** to the bundle's (same SHA-1 trailer) | fetch the bundle's 2,704 keep-around/merge-request refs into the clone, delete the bundle, make the clone bare (07) | 3 | 180.7 |
| 457.3 | `.zip`/`.rar` asset archives, 430 sets (146.6 inside `_Backups`) | | | |
| ↳ 75.6 | content already extracted on G: (≥ 90 % of members by name and size); 14.3 of it extracted only inside a backup | owner | 3 | 75.6 |
| ↳ 16.2 | `NoSafePlaces\Assets\LevelDesign\FireWatchTower.zip`, `HauntedPrison_5.0.zip` = the same files in `ToS\Assets - 3th\Leartes Studios` | keep one copy | 3 | 16.2 |
| ↳ 365.5 | the only copy on G: (Kitbash3D 60.7, ZBrush kits 59.6, Leartes 47.4, NoSafePlaces packs 115.6, ...) | move to an HDD (cold data) | 2 | 365.5 |
| 45.1 | 15 Visual Studio `.vs` folders | | | |
| ↳ 11.1 | IntelliSense PCH, browse DBs, Copilot and search indexes of solutions in git (`_Backups\…\2025\Client` 10.4) | new `visual-studio/intellisense-cache` (06-A) | 1 | 11.1 |
| ↳ 33.8 | the same caches, projects outside git (`_Backups\NetworkSample` 17.3, `NoSafePlaces\Client`, `UnderTheShadow`, `ToS\Client`, `PlayerController`, `ClientNetworkSample` ~3.3 each) | the same rule with `outside_git` (06-E, code) | 1b | 33.8 |
| ↳ 0.2 | per-user state (`.suo`, layouts) | `sln-state` (ask) | 3 | 0.2 |
| 28.8 | Rust `target`: `ToS\Server` 28.3 (26.8 on disk), a backup's `Benchmark` 0.5 | `rust/target` (existing, auto) | 1 | 28.8 |
| 43.7 | Unreal output (`Intermediate`, `DerivedDataCache`, logs, crashes, cook, stage, web cache) | | | |
| ↳ 11.7 | in git (the 2025 Client and 2024 AssetsNormalize backups) | `unreal/project-artifacts` (existing, ask) | 3 | 11.7 |
| ↳ 32.0 | outside git (`ToS\Client\Intermediate` 10.3, `NetworkSample` 5.2, `UnderTheShadow` 2.9, `PlayerController` 2.4, ...) | the same rule widened (06-B) with `outside_git` (06-E) | 3 | 32.0 |
| 27.0 | `G:\UE\Megascans Library`: every file also in a project (25.7 in `UnderTheShadow`) | owner; new attribution area (06-C) | 3 | 27.0 |
| 13.5 | Unreal `Saved\Autosaves` (9.3 in the 2025 Client backup) | owner | 3 | 13.5 |
| 6.5 | project `Binaries` and packaged `Build` outside git 3.7, `MutableSample` streamed data 2.8 | owner | 3 | 6.5 |
| 1.8 | `node_modules` (4) and .NET `bin` (4) in git repositories | existing auto rules | 1 | 1.8 |
| 284.7 | rest of `_Backups`: 2024 assets, 3D, design (117.8), 2025 Client content (58.4), PlayerController content (36.2), the 2023 `.git` (68.7), small servers | move to an HDD (cold data) | 2 | 284.7 |
| 438.1 | project content (`NoSafePlaces` 198.4, `UE_Projects` 61.8, `UnderTheShadow` 26.0), ZBrush library and the owner's assets (147.2), live sources (4.7) | — | – | |
| 3.2 | not visible: NTFS metadata, `System Volume Information`, `Config.Msi` | admin can size shadow storage | 3 | ? |

## Totals

| | GiB on this machine |
|---|---:|
| **(1) MyClean deletes on its own** (Rust `target` 28.8, of which 28.3 already counted; new VS cache rule 11.1; `node_modules` 1.5; .NET `bin` 0.4) | **41.7** (40.2 on disk) |
| **(1b) on its own after 06-E** (VS caches in projects outside git) | **33.8** |
| **(2) reversible: move to an HDD** (archives that are the only copy 365.5, the rest of `_Backups` 284.7) | **650.2** |
| **(3) owner decision** (2023 git copies 180.7, extracted archives 75.6, Unreal output 43.7 (ask), Megascans 27.0, duplicate archives 16.2, autosaves 13.5, binaries/builds 6.5, `.vs` state 0.2) | **363.3** |
| needed as it is | 438.1 |

The rows do not overlap: every file is counted once (01, the ledger), so the totals add up to
the 1,527.0 GiB the walk saw.

## Why the map explained 0 % of G:

Attribution areas name fixed places; G: has only owner-chosen project, backup and archive
folders. The 114.7 GiB of artifact folders are known to the project rules, which `myclean map`
does not use (08). The new Megascans area explains 27.0 GiB more.

## What changes in the knowledge packs (proposed, 06)

- **A** `visual-studio/intellisense-cache` and `slnx-intellisense-cache`: new, auto, targets
  `.vs\*\v1*\ipch`, `Browse.VC.db`, `Solution.VC.db` (+ journals), `CopilotIndices`,
  `FileContentIndex`; git-ignored, not in use, 7 days. 11.1 GiB on G: (data only, in spec).
- **B** `unreal/project-artifacts` gains `Saved\Cooked`, `Saved\StagedBuilds`, `Saved\webcache_*`
  (ask). 0.3 GiB on G: now.
- **C** `unreal/megascans-library` and **D** `windows/installer-rollback` (`?:\Config.Msi`):
  attribution areas.
- **E** (code + spec, needs approval): `require.outside_git`, so tool-owned paths in projects
  outside git still qualify; then A covers 44.8 GiB and Unreal output (with `Binaries` split
  into its own git-only case) 43.7 GiB on G:.

Drive wildcards: clean targets cannot use `?:\` (they resolve literally and silently match
nothing); attribution paths can. No proposal needs a G:-specific path (06).

Owner decisions with exact steps: 07. Code follow-ups: 08.
