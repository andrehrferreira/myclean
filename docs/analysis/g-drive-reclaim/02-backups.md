# 02 Backups (`G:\ToS\_Backups`, 676.3 GiB)

**Answer.** The backups are 44 % of G:. They are owner data, so MyClean does not delete them
as such. But **~246 GiB inside them is redundant or regenerable**: a 68.7 GiB git bundle whose
pack is byte-identical to a clone next to it, that clone's 112.0 GiB working tree (a checkout of
its own `.git`), 53.7 GiB of Visual Studio caches and Unreal/Rust/Node/.NET build output, and
11.8 GiB of archives whose content is already extracted next to them. Only 8.8 GiB of the backups also exist in the live
trees (by name and size), so the backups are mostly the only copy on G:. Nothing was written
after 2025-11-15: the whole folder is cold data for an HDD.

## What is inside

| GiB | Folder | Newest | What it is |
|---:|---|---|---|
| 249.54 | `Backup - ToS3\2023` | 2025-11-13 | `001.bundle` 68.67 (2023-05-02, with `001.refs` and `LATEST`), `repo-recuperado` 180.88 (a Unity project cloned from the bundle), `.git` (an empty repository: no commit) |
| 270.29 | `Backup - ToS3\2024` | 2025-05-14 | `Assets` 192.57 (Kitbash3D 60.69, UE packs 47.59, ArtStation 28.52, CGTrader 15.87, Polyphoria 13.23, Freshcaned 11.75, AdvancedTraversalSystem 6.92, ...), `3D` 41.15 (`Avatar` 31.34), `Design` 21.47, `AssetsNormalize` 13.19 (Unreal, in git), `Audio` 0.82, `Maps` 0.66, `ServerCSharp1.0` 0.22, `Dashboard` 0.17, three PDFs |
| 85.01 | `Backup - ToS3\2025` | 2025-11-13 | `Client` 84.85 (Unreal; in the git repository `2025`, 3 commits, last 2025-04-11), `Server` 0.16 |
| 41.99 | `UE - Projects\PlayerController` | 2025-07-24 | Unreal project, not in git: `Content` 36.21 (Paragon characters), `.vs` 3.31, `Intermediate` 2.41 |
| 25.78 | `NetworkSample` | 2025-11-15 | Unreal project, not in git: `.vs` 17.29, `Intermediate` 3.92, `Plugins` 2.06, `Saved` 1.46, `Binaries` 0.53, `Build` 0.50, `Content` 0.03 |
| 3.03 | `Server - LastCSharp` | 2025-09-05 | git clone of the live `G:\ToS\Server\ToS-Server-5` |
| 0.65 | `Backup - ToS1` | 2025-11-12 | `Server3` (git, 3,537 commits, last 2023-04-04), `Database` |
| 0.06 | `Server - LastTs` | 2025-03-11 | a NestJS server, not in git |

## The 2023 git copies: 180.7 GiB of the 249.5 are redundant

- `repo-recuperado\.git` holds one pack, `pack-2cd600d4…5750f.pack`, 73,729,470,054 bytes.
  `001.bundle` is 73,729,702,492 bytes: the same pack plus a 232,438-byte header, which is the
  ref list (`001.refs`, 232,421 bytes, is the same list as a file). The last 20 bytes of both
  files, the pack's SHA-1 trailer, are identical: `2cd600d4c11ce6c4c7952e57b26d2fbd4dd5750f`.
  So every object of the bundle is in the clone. The clone's `origin` is
  `/mnt/g/ToS/Backup - ToS3/2023/001.bundle`: it was cloned under WSL on 2025-11-13, before the
  folder moved into `_Backups`.
- The bundle has 2,716 refs: `HEAD`, 9 branches, 2 tags, 1,449 `refs/keep-around/*` and 1,255
  `refs/merge-requests/*` (a GitLab export). The clone has the branches and tags as refs. The
  keep-around and merge-request commits are in its pack but no ref points at them, so a
  `git gc` in the clone would eventually drop them.
- The working tree (`Assets` 110.96, `Packages` 1.22 and the rest: 111.99 GiB) is a checkout of
  `master` (last commit 2021-11-27, 1,054 commits). `git status` lists 19 modified files, all
  `.terrainlayer`/`.meta`/`.png.meta` files whose names contain parentheses, commas or
  apostrophes (checkout-side changes, not edits: nobody worked in this tree).

So the owner can keep the full history in 68.7 GiB instead of 249.5. The exact steps are in 07.

## Build output and caches inside the backups (53.7 GiB)

| GiB | Where | What | Rule | Selected today? |
|---:|---|---|---|---|
| 10.44 | `2025\Client\.vs` | IntelliSense PCH, browse DB | new `visual-studio/intellisense-cache` (auto) | yes, once added (git work tree `2025`, ignored by `Client/.gitignore:7`) |
| 17.26 | `NetworkSample\.vs` | ipch 9.89, browse DB 2.32 + 0.36, Copilot index 4.22 | same | only with `outside_git` (08) |
| 3.24 | `UE - Projects\PlayerController\.vs` | browse DB, Copilot index | same | only with `outside_git` |
| 0.52 | `Server - LastCSharp\.vs`, its `libsodium\.vs`, `2025\Server\.vs`, `ServerCSharp1.0\.vs`, `Server3\.vs` (all in git) | | same | yes |
| 6.77 | `2025\Client` `Intermediate` 5.69, `Binaries` 0.50, `DerivedDataCache` 0.22, `Saved\Logs`, `Saved\Crashes`, `Saved\webcache_4430` | Unreal output | `unreal/project-artifacts` (ask) | yes, ask (in git, ignored, inactive since 2025-11-13) |
| 4.91 | `2024\AssetsNormalize` `Intermediate` 4.72 (`PipInstall`), `DerivedDataCache`, `Saved\Crashes`, `Saved\webcache_4430` | Unreal output | `unreal/project-artifacts` (ask) | yes, ask |
| 7.58 | `NetworkSample` `Intermediate` 3.92, `Saved\Cooked` 0.62, `Saved\StagedBuilds` 0.50, logs and crashes 0.13; `UE - Projects\PlayerController\Intermediate` 2.41 | Unreal output | `unreal/project-artifacts` | only with `outside_git` |
| 1.03 | `NetworkSample\Binaries` 0.53, `NetworkSample\Build` 0.50 (packaged game) | | — | no (owner) |
| 1.08 | `Server - LastCSharp\Api\node_modules` 0.92, `2024\Dashboard\node_modules` 0.16 | | `node/artifacts` (auto) | yes |
| 0.52 | `Server - LastCSharp\Benchmark\target` | | `rust/target` (auto) | yes |
| 0.37 | `Backup - ToS1\Server3\GameServer\bin`, `Tests\bin`, `2024\ServerCSharp1.0\bin`, `Server - LastCSharp\bin` | | `dotnet/*-artifacts` (auto) | yes |

`NetworkSample` is 25.78 GiB, of which 23.66 is output and caches; its sources, config and
plugins are ~2.1 GiB.

## Autosaves and archives inside the backups

- `2025\Client\Saved\Autosaves` 9.25 GiB and `2024\AssetsNormalize\Saved\Autosaves` 0.56: the
  editor's autosaved packages. Owner (they are recovery copies, not output).
- Archives: 146.6 GiB of `.zip`/`.rar` in `2024\Assets` (Kitbash3D's ten `kb3d_*.zip` alone are
  60.69; three of them, 10.29 GiB, are the Unity HDRP variants of `medievalsiege`, while the
  project moved from Unity (2023) to Unreal). 11.83 GiB of these archives have their content
  extracted next to them in the same backup (04).

## Duplication

By name and byte size of every file of at least 1 MiB (660.4 GiB of the 676.3):

| GiB | Also exists |
|---:|---|
| 8.76 | in a live tree outside `_Backups` (5.43 of it from `2024\Assets`, e.g. Zahra's armour textures also in `G:\ToS\_Base\Meshs\Armors`) |
| 8.37 | only in another backup folder |
| 19.41 | twice or more inside the same backup folder (`2024\3D` 7.25, `2023\repo-recuperado` 6.58, `2024\Assets` 2.28, `UE - Projects` 2.12, ...) |

Two pairs are exact copies (sampled SHA-1): `2024\Design\2024\UI5-Dani\tos_HUD_2024_ingame (1).psd`
= `tos_HUD_2024_ingame.psd` (1.91 GiB) and the two `Dragon Temple … UE 5.1 - 5.3.rar` spellings
(0.60 GiB).

**Not duplicates of the live trees:** `2025\Client` has 58.26 GiB of `Content` (Brushify 21.18,
ErrantWorlds 14.43, MM_Terrains 13.19, ...), while the live `G:\ToS\Client\Content` has 0.13 GiB.
`Server - LastCSharp` is at the same commit as the live `ToS-Server-5` (`e8c1056`) but has 5
uncommitted changes (two modified files, a moved `Transpilers` folder), so it is not a pure copy
either.
