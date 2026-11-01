# E: drive reclaim: summary

**Answer.** E: ("Tools & Games", 953.9 GiB, 795.3 used, 158.6 free) can get back about **177
GiB without asking anyone** as soon as one new rule ships, and about **119 GiB more on its own
within three weeks** if the projects stay idle. That is ~296 GiB, more than a third of the
drive. About **176 GiB more is reversible** (moving the two Steam games to another drive,
compression), and up to about **240 GiB more if the owner decides** (scratch folders, the
Recycle Bin, worktrees, datasets, clones).

One thing dominates: **Zig's project build cache, `.zig-cache`: 246 GiB in 16 folders**, 117
GiB in UzEngine's `runtime` alone. Zig never trims it, its docs say it "can be completely
deleted at any time with no consequences", and no pack has a rule for it. The proposed
`zig/project-cache` (07) deletes it once its project has been idle for 7 days, like Rust's
`target`.

Measured read-only on 2026-11-01 from the given map (08:16) and targeted reads (01). Sizes are
GiB.

## Where the 795 GiB go, and what to do

Status: **(1)** MyClean deletes it on its own (tier `auto`, 100 % regenerable), **(2)**
reversible move or compression, **(3)** only the owner decides. "–" means it is needed as it is.

| GiB | What | Action | Status | Reclaim GiB |
|---:|---|---|:-:|---:|
| 118.1 | `UzEngine\runtime\.zig-cache` 116.6 and 4 demo caches 1.5 (project idle 9 d) | `zig/project-cache` (new, auto, 7 idle days) | 1 | 118.1 |
| 73.7 | `UzPainter\.zig-cache` (idle 5 d) | same rule, from 2026-11-03 if still idle | 1 | 73.7 |
| 77.4 | 8 Claude Code agent worktrees in `UzEngine\.claude\worktrees` (all merged into `main`) + `fix-vfx-pr7` | their `.zig-cache` 49.5 (new rule, now), their `runtime\build` 14.2 (`cpp/cmake-artifacts`, 30 d); `git worktree remove` for the rest | 1 / 3 | 63.7 / 13.7 |
| 23.7 | `UzEngine\runtime\build` (31 `readiness-*` build trees) | `cpp/cmake-artifacts` (existing), ~2026-11-22 | 1 | 23.7 |
| 13.8 | `UzEngine\.sandbox\stabilization` (2026-10-10) | `.zig-cache` 4.4 now, builds 3.7 at 30 d; the rest | 1 / 3 | 8.2 / 5.6 |
| 140.4 | **Path of Exile 2** (never started from Steam) | move to D: in Steam, or uninstall | 2 / 3 | 140.4 |
| 31.2 | No Man's Sky (last played 2026-09-20) | move or uninstall | 2 / 3 | 31.2 |
| 1.2 | Blender (Steam) 0.9, shader pre-cache 0.17 | `games/steam-library-shadercache` (new, needs code 09-1) | – / 1 | 0.17 |
| 82.5 | **Creature `Temp` folders** in Assets3D (Lich 62.2, untouched since 10-18) | owner; optional owner pack (`ask`, quarantine) | 3 | 82.5 |
| 49.1 | **Recycle Bin**: `HiveLLM\Synap\target` 41.75, a UzPainter copy 6.5 | approve `windows/recycle-bin` (ask) | 3 | 49.1 |
| 23.9 | `UzPainter\scripts\tmp` (agent runs of 10-23; the repo now forbids the folder) | owner, after grepping the task notes | 3 | 23.9 |
| 74.9 | Assets3D creature projects and models 68.0, `_tmp`, Unreal, Pipeline, Toolbox, Docs, Tools | `.spp` ×3 duplicate 3.9, Painter/Blender backups 7.1, `_tmp` 1.8; `Toolbox\out` 1.0 (cmake, 30 d) | 3 / 1 | 12.8 / 1.0 |
| 45.0 | HiveLLM (24 projects) | `node_modules` of idle projects 3.0 (existing auto); TML build and submodules 18.3, Expert venv and models 10.2, datasets 6.3 | 1 / 3 | 3.0 / 34.8 |
| 19.0 | the rest of UzEngine: `runtime\thirdparty` 3.5, `sample` 3.1, `.screenshots` 2.8, `.rulebook` 2.5, `.git` 2.1, `.tools` 1.4, `cache` 1.4, `extension` 0.8, small builds 0.4 … | screenshots, March memory index, staging leftovers (owner); vcpkg clone 0.43 (auto) | 3 / 1 / – | 8.9 / 0.4 |
| 14.6 | Elyriu 5.9, Falcor 3.7, `E:\tmp` 2.8, Godot 2.1, Options 0.3 | `node_modules` 0.9 (auto); clones, paused project, scratch (owner) | 1 / 3 | 0.9 / 13.7 |
| 2.6 | UzPainter `zig-out` 2.2 and the rest of UzPainter | – | – | |
| ~4.5 | `.obj` meshes and dataset text, inside the rows above | LZX compression | 2 | 4.5 |
| 4.0 | not visible (NTFS metadata, system folders, 48 folders the owner cannot list) | – | – | |

## Totals

| | GiB on this machine |
|---|---:|
| **(1) MyClean deletes on its own, now** (new `zig/project-cache` 172.1; existing `node/artifacts` 3.9, `vcpkg/clone-build-files` 0.43, `rust/target` 0.06, `dotnet` 0.04) | **~177** |
| **(1) on its own later, if the projects stay idle** (UzPainter `.zig-cache` 73.7 on 2026-11-03; CMake and MSBuild output 43.1 around 2026-11-22; `node_modules` 2.1) | **~119** |
| **(2) reversible** (Steam games moved to another library 171.6, compression ~4.5) | **~176** |
| **(3) owner decision** (creature Temp 84.3 with `_tmp`, Recycle Bin 49.1, `scripts\tmp` 23.9, HiveLLM 34.8, worktrees 13.7, backups and duplicates 11.0, UzEngine tooling 8.9, sandbox 5.6, clones, Elyriu and scratch 13.7) | **~245** (≤7 overlap: some Painter autosaves are inside Temp) |

The games are counted once, under (2); uninstalling them instead (3) frees the same 171.6.
Everything else is counted once. What remains (~79 GiB) is needed: creature projects and
models, engine sources, sample content and git histories.

## What changes in the knowledge packs (07)

- **New `zig/project-cache`** (auto, user, project marker `build.zig`, targets `.zig-cache` and
  `zig-cache`, git-ignored, not in use, 7 idle days), an attribution area for `.zig-cache` at
  every scanner depth, `zig` 1.2.0, a fixture with E:'s folder names, one spec sentence. Data
  only: 172 GiB here now.
- **New `games/steam-library-shadercache`** (auto, 0.17 GiB here): written, but clean targets
  cannot name `?:\` today, so it waits for 09-1.
- No other rule change. The existing project rules already reach E: (workspace roots now include
  it) and would clean ~4.5 GiB now and ~45 GiB more as projects pass 30 idle days.

Code needed (09): clean targets on every drive or per Steam library; the map explaining
workspace projects (the reason 72 % of E: is "unexplained"); Recycle Bin by deletion age; a
target that is the marker's own folder (`CMakeCache.txt`, `CACHEDIR.TAG`); merged worktrees as
a finding. Owner decisions with exact actions: 08.
