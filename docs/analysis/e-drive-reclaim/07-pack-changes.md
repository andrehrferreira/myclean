# 07 Proposed knowledge pack changes

Proposals only: this analysis changed no file under `data/packs/`. Each auto case has
`not_in_use` and a wait of at least 7 days, as the cleaning spec requires ("An `auto`-tier rule
MUST wait at least 7 days").

| Case | Tier | Scope | GiB on E: | Ready |
|---|:-:|:-:|---:|---|
| `zig/project-cache` (new clean rule + attribution area) | auto | user | 172.1 now, +73.7 once UzPainter is idle 7 days | data, spec sentence, docs row |
| `games/steam-library-shadercache` (new clean rule + area) | auto | user | 0.17 | needs the engine change 09-1 first |
| `windows/recycle-bin` | ask (unchanged) | user | 49.1 | no change (04) |

## 1. `zig/project-cache` (new)

### `data/packs/zig/clean.toml` (append)

```toml
# The local cache of a Zig project (source: Zig build system guide, see pack.toml): zig build
# keeps it in .zig-cache beside build.zig (zig-cache before Zig 0.13), and it "can be completely
# deleted at any time with no consequences"; the next build compiles again. Zig never trims it
# (observed on reference machine: 246 GiB in 16 caches on E:, 117 GiB in one). The scanner walks
# the roots of data/workspaces.toml; a folder holding build.zig is a project root. The whole
# cache is proposed only when git ignores it and tracks nothing inside it, no process uses it or
# the project, and the project is inactive for 7 days (newest of the last commit and every source
# file), as for Cargo's target. Single h or o entries are never targets: a manifest left without
# its output can make zig use a missing file. zig-out, the install prefix, is never a target:
# what goes there is the project's choice.
[[rule]]
id       = "project-cache"
category = "dev-artifacts"
tier     = "auto"
scope    = "user"
strategy = "delete"
project  = { marker = "build.zig" }
targets  = [".zig-cache", "zig-cache"]
require  = { git_ignored = true, not_in_use = true, inactive_days = 7 }
```

### `data/packs/zig/attribution.toml` (append)

Attribution paths cannot say "at any depth", so the area names the folder at each depth the
workspace scanner reaches (`max_depth = 8`). `.zig-cache` is a name only Zig uses. The old name
`zig-cache` is left out of the area on purpose: a project-pinned global cache can carry it too
(`E:\UzmiGames\UzEngine\.tools\zig-cache`), and the area would mislabel it.

```toml
# Project-local build caches (source: Zig build system guide, see pack.toml). Projects can be
# anywhere, so the folder is named at every depth the workspace scanner walks (data/workspaces.toml,
# max_depth 8). Observed on reference machine: E:\UzmiGames\UzEngine\runtime\.zig-cache (117 GiB),
# E:\UzmiGames\UzEngine\.claude\worktrees\agent-*\runtime\.zig-cache.
[[area]]
id       = "project-cache"
paths    = ['?:\.zig-cache', '?:\*\.zig-cache', '?:\*\*\.zig-cache', '?:\*\*\*\.zig-cache',
            '?:\*\*\*\*\.zig-cache', '?:\*\*\*\*\*\.zig-cache', '?:\*\*\*\*\*\*\.zig-cache',
            '?:\*\*\*\*\*\*\*\.zig-cache', '?:\*\*\*\*\*\*\*\*\.zig-cache']
category = "build-artifacts"
label    = "Zig project build caches (.zig-cache)"
reclaim  = { kind = "clean-rule", ref = "project-cache", note = "Deleted on its own once the project is inactive for 7 days; zig build compiles again." }
```

A better long-term answer is for the map to attribute project artifacts through the workspace
scanner (09-2); then this area can go.

### `data/packs/zig/pack.toml`

```toml
id          = "zig"
version     = "1.2.0"
title       = "Zig"
description = "Zig's global build cache (compiled outputs, hash manifests and ZIR caches that zig rebuilds when missing) and the .zig-cache of Zig projects inactive for 7 days."
sources     = [
  "https://github.com/ziglang/zig/blob/master/lib/std/zig/introspect.zig (resolveGlobalCacheDir: ZIG_GLOBAL_CACHE_DIR, else %LOCALAPPDATA%\\zig)",
  "observed on reference machine: %LOCALAPPDATA%\\zig holds b, h, o, tmp and z; zig recompiles entries that are gone",
  "https://ziglang.org/learn/build-system/ (zig build creates .zig-cache beside build.zig: it \"can be completely deleted at any time with no consequences\"; zig-out is the installation prefix, its content the project's choice)",
  "https://ziglang.org/download/0.13.0/release-notes.html (the local cache folder zig-cache was renamed .zig-cache)",
  "observed on reference machine: 246 GiB in 16 .zig-cache folders on E: (E:\\UzmiGames\\UzEngine\\runtime\\.zig-cache 117 GiB with 20,743 o entries, E:\\UzmiGames\\UzPainter\\.zig-cache 74 GiB, eight agent worktrees 4.7-7.7 GiB each), all ignored by git; Zig never trimmed them",
]
min_myclean = "0.1.0"

[applies_when]
any = [{ path_exists = '%LOCALAPPDATA%\zig' }, { path_exists = '%ZIG_GLOBAL_CACHE_DIR%' }]
```

(`applies_when` is unchanged: every `zig build` also creates the global cache, so a machine
with project caches has `%LOCALAPPDATA%\zig` too. `min_myclean` stays: project rules and `?:`
in attribution are older.)

### `data/packs/zig/tests/projects.toml` (new fixture)

```toml
# Zig projects in a workspace, with the reference machine's folder names (E:). An inactive
# project's ignored .zig-cache goes, with those of its demos and of an agent worktree; an active
# project keeps its cache; zig-out, a cache git does not ignore and a cache without build.zig
# beside it stay.

[workspace]
roots = ['E:\']

# UzEngine: last source change 9 days ago.
[[entry]]
path = 'E:\UzmiGames\UzEngine\.git'
folder = true
[[entry]]
path = 'E:\UzmiGames\UzEngine\runtime\build.zig'
age_days = 9
[[entry]]
path = 'E:\UzmiGames\UzEngine\runtime\engine\renderer.zig'
age_days = 9
[[entry]]
path = 'E:\UzmiGames\UzEngine\runtime\.zig-cache'
folder = true
git_ignored = true
[[entry]]
path = 'E:\UzmiGames\UzEngine\runtime\.zig-cache\o\255b6c40b8517e9e7f3bf461b43ebe4f\runtime.lib'
size = 751619276
age_days = 9
[[entry]]
path = 'E:\UzmiGames\UzEngine\runtime\.zig-cache\h\timestamp'
age_days = 9
[[entry]]
path = 'E:\UzmiGames\UzEngine\runtime\demos\triangle\build.zig'
age_days = 200
[[entry]]
path = 'E:\UzmiGames\UzEngine\runtime\demos\triangle\main.zig'
age_days = 200
[[entry]]
path = 'E:\UzmiGames\UzEngine\runtime\demos\triangle\.zig-cache'
folder = true
git_ignored = true
[[entry]]
path = 'E:\UzmiGames\UzEngine\runtime\demos\triangle\.zig-cache\o\8f1d2c3b4a5968778695a4b3c2d1e0f1\triangle.exe'
size = 16777216
age_days = 200
[[entry]]
path = 'E:\UzmiGames\UzEngine\runtime\demos\triangle\zig-out'
folder = true
git_ignored = true
[[entry]]
path = 'E:\UzmiGames\UzEngine\runtime\demos\triangle\zig-out\bin\triangle.exe'
size = 16777216
age_days = 200
# A project-pinned global cache: no build.zig beside it.
[[entry]]
path = 'E:\UzmiGames\UzEngine\.tools\zig-cache\o\0a1b2c3d4e5f60718293a4b5c6d7e8f9\std.o'
age_days = 22

# A Claude Code agent worktree of UzEngine (its .git is a file on disk; a folder here).
[[entry]]
path = 'E:\UzmiGames\UzEngine\.claude\worktrees\agent-a1ea4dfd25f07d406\.git'
folder = true
[[entry]]
path = 'E:\UzmiGames\UzEngine\.claude\worktrees\agent-a1ea4dfd25f07d406\runtime\build.zig'
age_days = 9
[[entry]]
path = 'E:\UzmiGames\UzEngine\.claude\worktrees\agent-a1ea4dfd25f07d406\runtime\.zig-cache'
folder = true
git_ignored = true
[[entry]]
path = 'E:\UzmiGames\UzEngine\.claude\worktrees\agent-a1ea4dfd25f07d406\runtime\.zig-cache\o\4c2f9a0b1c2d3e4f5a6b7c8d9e0f1a2b\runtime.lib'
size = 734003200
age_days = 9

# An old worktree whose .gitignore did not list .zig-cache yet.
[[entry]]
path = 'E:\UzmiGames\UzEngine\.claude\worktrees\fix-vfx-pr7\.git'
folder = true
[[entry]]
path = 'E:\UzmiGames\UzEngine\.claude\worktrees\fix-vfx-pr7\runtime\build.zig'
age_days = 213
[[entry]]
path = 'E:\UzmiGames\UzEngine\.claude\worktrees\fix-vfx-pr7\runtime\.zig-cache\o\9a8b7c6d5e4f30211203f4e5d6c7b8a9\runtime.lib'
age_days = 213

# UzPainter: a source changed yesterday.
[[entry]]
path = 'E:\UzmiGames\UzPainter\.git'
folder = true
[[entry]]
path = 'E:\UzmiGames\UzPainter\build.zig'
age_days = 5
[[entry]]
path = 'E:\UzmiGames\UzPainter\src\main.zig'
age_days = 1
[[entry]]
path = 'E:\UzmiGames\UzPainter\.zig-cache'
folder = true
git_ignored = true
[[entry]]
path = 'E:\UzmiGames\UzPainter\.zig-cache\o\3eb303f6fe45043d576a718883419a9f\uzpainter.dll'
size = 322122547
age_days = 5

[[expect]]
kind     = "clean"
case     = "project-cache"
match    = ['E:\UzmiGames\UzEngine\runtime\.zig-cache',
            'E:\UzmiGames\UzEngine\runtime\demos\triangle\.zig-cache',
            'E:\UzmiGames\UzEngine\.claude\worktrees\agent-a1ea4dfd25f07d406\runtime\.zig-cache']
no_match = ['E:\UzmiGames\UzPainter\.zig-cache', 'E:\UzmiGames\UzEngine\runtime\demos\triangle\zig-out',
            'E:\UzmiGames\UzEngine\.claude\worktrees\fix-vfx-pr7\runtime\.zig-cache',
            'E:\UzmiGames\UzEngine\.tools\zig-cache', 'E:\UzmiGames\UzEngine\runtime\build.zig',
            'E:\UzmiGames\UzEngine\runtime\.zig-cache\o', 'E:\UzmiGames\UzEngine\runtime\engine']

[[expect]]
kind     = "attribution"
case     = "project-cache"
match    = ['E:\UzmiGames\UzEngine\runtime\.zig-cache', 'E:\UzmiGames\UzPainter\.zig-cache',
            'E:\UzmiGames\UzEngine\.claude\worktrees\agent-a1ea4dfd25f07d406\runtime\.zig-cache']
no_match = ['E:\UzmiGames\UzEngine\runtime', 'E:\UzmiGames\UzEngine\runtime\demos\triangle\zig-out',
            'E:\UzmiGames\UzEngine\.tools\zig-cache']
```

### Around it

- Cleaning spec (`.rulebook/tasks/phase2_free-c-drive/specs/cleaning/spec.md`, "Developer
  Artifact Qualification"), after the Cargo sentence: "A Zig project's `.zig-cache` goes after
  7 inactive days (the Zig build-system guide: it can be deleted at any time; the next build
  compiles again)." `catalog_test.cpp` `ArtifactRulesRequireEveryQualification` already
  accepts it (auto, `git_ignored`, `not_in_use`, 7 days).
- `docs/packs.md`, Shipped packs, `zig` row: "clean: global cache entries, the `.zig-cache` of
  Zig projects inactive 7 days; attribution: global cache, project caches".
- Checks to run with the change: `myclean packs validate data\packs\zig`, then
  `ctest -L packs|clean|rules`.

## 2. `games/steam-library-shadercache` (new; needs 09-1)

```toml
# Steam's per-library shader pre-cache (source: the Steam client's Settings > Downloads > Shader
# Pre-Caching, which downloads and builds these per game; observed on reference machine:
# E:\SteamLibrary\steamapps\shadercache\275850, No Man's Sky, 0.17 GiB):
# shaders Steam downloads again or the game compiles again, one folder per app id. The library
# under Program Files is dropped by the deny-list, as for steam-httpcache.
[[rule]]
id       = "steam-library-shadercache"
category = "gpu-shader-caches"
tier     = "auto"
scope    = "user"
strategy = "delete"
targets  = ['?:\SteamLibrary\steamapps\shadercache\*']
require  = { min_age_days = 7, not_in_use = true }
```

```toml
[[area]]
id       = "steam-library-shadercache"
paths    = ['?:\SteamLibrary\steamapps\shadercache', '%ProgramFiles(x86)%\Steam\steamapps\shadercache']
category = "app-caches"
label    = "Steam shader pre-cache"
reclaim  = { kind = "clean-rule", ref = "steam-library-shadercache" }
```

`?:\SteamLibrary` is Steam's default folder name on a second drive, not every library. With
09-1 in its better form (a `%STEAM_LIBRARY%` target expanded from `libraryfolders.vdf`), the
target becomes `'%STEAM_LIBRARY%\steamapps\shadercache\*'` and covers libraries of any name.
`not_in_use` matters: Steam and the game keep the cache open while it runs.

Fixture (`data/packs/games/tests/launchers.toml`, added with the code change):

```toml
[[entry]]
path = 'E:\SteamLibrary\steamapps\shadercache\275850\fozpipelinesv6\steamapprun_pipeline_cache.foz'
size = 182452224
age_days = 29
[[entry]]
path = 'E:\SteamLibrary\steamapps\shadercache\365670\fozpipelinesv6\steamapprun_pipeline_cache.foz'
age_days = 2
[[entry]]
path = 'E:\SteamLibrary\steamapps\appmanifest_275850.acf'
[[entry]]
path = "E:\\SteamLibrary\\steamapps\\common\\No Man's Sky\\Binaries\\NMS.exe"

[[expect]]
kind     = "clean"
case     = "steam-library-shadercache"
match    = ['E:\SteamLibrary\steamapps\shadercache\275850']
no_match = ['E:\SteamLibrary\steamapps\shadercache\365670', 'E:\SteamLibrary\steamapps\shadercache',
            'E:\SteamLibrary\steamapps\appmanifest_275850.acf', "E:\\SteamLibrary\\steamapps\\common\\No Man's Sky"]
```

(The inner file names of `shadercache\275850` were not listed in this analysis; the layout is
Steam's Fossilize cache. Bump `games` to 1.2.0 and add the Shader Pre-Caching source to its
`sources`. Check the claim "Steam downloads or rebuilds a missing pre-cache" against Valve's
documentation before shipping; this analysis did not find a Valve page that says it.)

## Considered and not proposed

| Candidate | GiB on E: | Why not |
|---|---:|---|
| stale `h`/`o` entries of active Zig projects | (of 73.7 + 116.6) | not 100 % safe one by one (02); the whole cache goes after 7 idle days instead |
| `zig-out` | 2.3 | install prefix; the project decides its content |
| `Assets3D\Creatures\*\*\Temp` | 82.5 | owner scratch that may hold the only copy of a fix; one repository's layout (08 has an optional owner pack) |
| `UzPainter\scripts\tmp` | 23.9 | the repository says to check its task notes before deleting (08) |
| a generic "folder with `CMakeCache.txt`" rule (Falcor, Tml's LLVM build) | 3.3 + 1.4 | the engine cannot target the folder that holds the marker (09-4); Falcor's `build` is not ignored by git |
| Recycle Bin items older than 30 days | 0 now | the deletion time is only in the `$I` record (09-3) |
| Wardogs' leftover `.sentry-native` in `steamapps\common` | 0.00 | no space |
