# 04 Download archives, the Megascans library and duplicates

**Answer.** G: holds **457.3 GiB of `.zip`/`.rar`/`.7z` archives** (430 sets, multi-part sets
counted once): purchased or downloaded asset packs, almost all untouched since mid-2025.
**75.6 GiB** of them have their content already extracted on G: (at least 90 % of the bytes of
their members of 1 MiB or more, by name and size), **16.2 GiB** are a second byte-identical copy
of another archive, and the other **365.5 GiB** are the only copy on G:. Separately, the
**Megascans library (27.0 GiB)** is entirely duplicated inside the projects that use it. None of
this is a cleaning case: these are the owner's purchases. They are owner decisions (07), and the
archives are the best candidates for the cold-data move to an HDD.

## Archives by place

| GiB | Sets | Place | Already extracted on G: |
|---:|---:|---|---:|
| 146.57 | 176 | `_Backups\Backup - ToS3\2024` (Assets: Kitbash3D 60.69, UE packs, ArtStation, CGTrader, Polyphoria, Freshcaned) | 11.83 |
| 79.14 | 86 | `_Base\Bases ZBrush` (OBJ/FBX zips next to the extracted ZTL/OBJ) | 19.51 |
| 62.62 | 16 | `NoSafePlaces\Assets\LevelDesign` | 4.24 (+16.17 duplicate copies) |
| 47.42 | 8 | `Assets - 3th\Leartes Studios` | 0 |
| 29.27 | 13 | `NoSafePlaces\Assets\Characters` | 9.08 |
| 27.98 | 16 | `NoSafePlaces\Assets\Volodymyr Stepaniuk` | 0 |
| 15.24 | 5 | `Assets - 3th\ErrantPhoton` | 14.27 (only inside the 2025 backup) |
| 12.11 | 17 | `_Base\Brushes` | 9.24 |
| 10.01 | 23 | `NoSafePlaces\Assets\Weapons` | 0.81 |
| 6.90 | 1 | `NoSafePlaces\Assets\Bundles` (Apocalypse bundle) | 0 |
| 4.51 | 2 | `NoSafePlaces\AssetsNormalization\Content` (`AutoRockCliffs.rar`, `PCGGeometryDecals.rar`, next to their extracted folders) | 4.51 |
| 3.44 / 2.73 / 1.72 | 24 | `NoSafePlaces\Assets\Tools`, `Animations`, `SoundDesign` | 1.21 |
| 2.65 / 1.25 / 0.68 | 7 | `Assets - 3th\Naked Singularity Studio`, `Marcus Whinney`, `Easy` | 1.25 |
| 1.58 + 0.80 | 7 | others (`NoSafePlaces\Assets` root, `_Base\Meshs`, creatures, 2023/2025 backups) | 0.35 |
| **457.26** | **430** | | **75.61** |

How "already extracted" was measured: each archive's member list (zip central directory or rar
headers, nothing extracted) was compared with every file of at least 1 MiB on G:, by file name
and uncompressed size. It is a lower bound: content that was imported into a project and saved
again by Unreal (`.uasset` re-serialized) no longer matches. Where the extracted copy lives:

- 41.7 GiB: archive and extracted copy both in live trees (e.g. `_Base\Bases ZBrush\DEMON part 2 43 Torsos\DEMON-TORSOS-OBJ.zip` next to its OBJs; `NoSafePlaces\Assets\LevelDesign\Outpost and Bunker Environment UE 5.0 - 5.1.zip` → `NoSafePlaces\Client\Content`; `Modular_Military_Character_UE_5.0_-_5.6` (5 parts, 9.08) → `NoSafePlaces\Client\Content`).
- 14.3 GiB: archive live, extracted copy **only in a backup** (`ErrantLandscapeStampLibrary.zip` 13.79 and `ErrantWorlds-BasicLandscapeMaterial.zip` 0.48 → `_Backups\Backup - ToS3\2025\Client\Content\ErrantWorlds`). Deleting these archives leaves the backup as the only copy.
- 8.9 GiB: both inside `2024` backup (ArtStation Zahra/Alma rars, Polyphoria, AdvancedTraversalSystem zips, a Google Drive download zip).
- 1.4 GiB: archive in the backup, extracted live (`Modular_Dungeon … UE_5.1_-_5.3.rar` → `UE_Projects\AssetNormalize\Content`).
- the rest of the 75.6: later volumes of multi-part sets above.

## Byte-identical copies (sampled SHA-1)

Files of at least 64 MiB with the same size, confirmed by hashing three 1 MiB samples: **48.9 GiB
of second copies** in all.

| GiB of extra copies | What |
|---:|---|
| 22.58 | `G:\UE\Megascans Library\Downloaded\UAssets\<id>\Tier0\*.uasset` = `G:\UnderTheShadow\Content\Megascans\…` (147 files) |
| 16.17 | `NoSafePlaces\Assets\LevelDesign\FireWatchTower.zip` (11.68) and `HauntedPrison_5.0.zip` (4.49) = the same files in `ToS\Assets - 3th\Leartes Studios` |
| 2.50 | inside `_Backups\…\2024`: `tos_HUD_2024_ingame (1).psd` (1.91), two spellings of `Dragon Temple … UE 5.1 - 5.3.rar` (0.60) |
| 2.31 | `UE_Projects\ClothTest55`: the packaged game three times (`Binaries`, `Saved\StagedBuilds`, `Build\Windows`) |
| 1.53 | Zahra armour textures in `_Base\Meshs\Armors\Zahra.3D` and `_Backups\…\2024\Assets\ArtStation\Zahra` |
| 0.81 | `_Backups\NetworkSample`: packaged game in `Binaries`, `Saved\StagedBuilds`, `Build`; shader archives in `Saved\Shaders` and `Saved\Cooked` |
| 0.73 | `ToS\Server\target`: Cargo's hard links (`deps` and the output folder), not real copies |
| 0.56 | Megascans library = `NoSafePlaces\Client\Content` (8 files) |
| 0.53 | `Assets - 3th\Marcus Whinney\*\HighPoly-Model-V2-0-UPDATE-Tutorial.mp4`, the same video in 5 model folders |
| 0.51 | `Assets - Custom` (3 pairs) |
| 0.65 | others (`repo-recuperado` textures, `2025\Client`, `NoSafePlaces\Assets`) |

MyClean's `scan-all` dedupe check would find most of these as well; they are listed here so the
owner sees them with the C: and E: findings.

## The Megascans library (`G:\UE\Megascans Library`, 26.96 GiB)

Quixel Bridge's local library, set to `G:\UE\Megascans Library`: `Downloaded\UAssets\<asset id>\Tier0`
holds the Unreal-ready assets Bridge downloaded (newest 2026-03-07). When Bridge adds an asset to a
project it copies it into the project's `Content\Megascans`. Comparing every file of at least 1 MiB
(26.84 of the 26.96 GiB): **all of it also exists in a project** (25.73 in
`UnderTheShadow\Content\Megascans`, 0.90 in `NoSafePlaces\Client`, 0.21 in `ClothTest55`). So
the library is a pure second copy on this machine today.

It is still not a 100 %-safe auto case: the library is also what Bridge offers to add to the next
project, and since the Megascans move to Fab (2024) re-downloading depends on the owner's account
and licences. Status 3 (owner), with a new attribution area so the map explains it (06).

## Not duplicates

- `G:\ToS\_Base` (181.6 GiB): ZBrush base meshes (`.ztl`, VDM brushes `.zbp`), OBJ/FBX and
  textures, plus their archives (79.1 + 12.1 GiB, 28.8 already extracted beside them). Owner's
  reference library.
- `G:\NoSafePlaces\Client\Content` (176.1 GiB), `UE_Projects\AssetNormalize\Content` (45.0),
  `NoSafePlaces\AssetsNormalization\Content` (19.5), `UnderTheShadow\Content` (25.9, of which
  25.75 Megascans): project content, mostly imported marketplace packs. 05.
