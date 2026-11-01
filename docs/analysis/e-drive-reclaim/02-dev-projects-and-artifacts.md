# 02 Development projects and their build output

E: holds the owner's game-engine work (`E:\UzmiGames`, 509.8 GiB) and the HiveLLM projects
(`E:\HiveLLM`, 45.0 GiB). Build output is the largest regenerable part of the drive, and one
tool dominates it: **Zig's project-local cache, `.zig-cache`, 246 GiB in 16 folders**. No
pack has a rule for it today, so nothing on E: is cleaned there. The C++/CMake build folders
(43 GiB) are covered already but wait for their 30 days of inactivity.

The rule emulation below ran the project-artifact rules of the packs over E: (`eproj.py`, 01): the same markers and targets, `git check-ignore` on each folder, the last commit of its
work tree and the newest source file outside build folders. Sizes are bytes of the files
(pnpm hard links counted once per link, so `node_modules` is slightly over-stated).

## Zig: `.zig-cache` (no rule yet; proposed `zig/project-cache`, 07)

`zig build` keeps its local cache in `.zig-cache` beside `build.zig` (`zig-cache` before Zig
0.13). The Zig build-system guide says it "can be completely deleted at any time with no
consequences". Zig never trims it: each changed build adds new `o\<hash>` entries, so it only
grows. UzPainter's own rules also say that editing a source during `zig build` "poisons
`.zig-cache`" (its decision 041); deleting the cache is the fix.

| GiB | Folder | Project idle | Git ignores it | With the proposed rule (7 days) |
|---:|---|---:|:-:|---|
| 116.6 | `UzEngine\runtime\.zig-cache` (20,743 `o` entries; 45 GiB older than 30 days) | 9 d | yes | auto now |
| 73.7 | `UzPainter\.zig-cache` (6,953 `o` entries, newest 2026-10-27) | 5 d | yes | auto from 2026-11-03 if still idle |
| 49.5 | 8 Claude Code agent worktrees `UzEngine\.claude\worktrees\agent-*\runtime\.zig-cache` (4.7–7.7 each) | 9–22 d | yes | auto now |
| 4.4 | `UzEngine\.sandbox\stabilization\native-clean-checkout\runtime\.zig-cache` | 9 d (22 d for the sandbox itself) | yes (`.sandbox/`) | auto now |
| 1.5 | `UzEngine\runtime\demos\{triangle,vfx_viewport,vfx_viewport_app,common}\.zig-cache` | 9 d | yes | auto now |
| 0.3 | 16 small caches in the `fix-vfx-pr7` worktree (2026-04) | 213 d | **no** (old `.gitignore`) | never (not ignored) |
| **246.1** | | | | **172.1 now, 73.7 in 2 days** |

Not proposed: `zig-out` (`UzPainter\zig-out` 2.2, `demos\triangle\zig-out` 0.08). The guide
calls it an installation prefix whose content "you, as the project maintainer, pick", so it can
be the only copy of what a project ships. `UzEngine\.tools\zig-cache` (0.12) and
`zig-native-clean-cache` (0.09) are the global caches of a project-pinned Zig, beside no
`build.zig`; they are left with `.tools` (owner, 08).

Why the whole folder and not stale entries of active projects (like `rust/stale-incremental`):
Zig keeps manifests in `h\` and outputs in `o\`. A manifest that still hits while its `o` entry
is gone can make Zig use a missing file, and their ages are not tied (Zig can rewrite a manifest
on a cache hit, for example when an input's timestamp changed, without touching the output). Deleting `h` and `o` entries one by one
is therefore not 100 % safe; deleting the whole folder is what the Zig docs allow. The same
caution applies to the shipped `zig/global-cache` rule, which deletes `o`, `h` and `z` entries
one by one (09).

## C/C++: CMake and MSBuild output (existing rules, not due yet)

| GiB | Folder | Project idle | Rule | Due |
|---:|---|---:|---|---|
| 23.7 | `UzEngine\runtime\build` (31 `readiness-*` build trees of ~0.9 GiB, `bin`, `lib`) | 9 d | `cpp/cmake-artifacts` (auto, 30 d) | ~2026-11-22 |
| 14.2 | `runtime\build` of the 8 agent worktrees (1.7–1.8 each) | 9–22 d | `cpp/cmake-artifacts` | 2026-11-09 to 11-22 |
| 1.9 + 0.3 | `.sandbox\…\native-clean-checkout\runtime\build`, `…\napi\build` | 9 d | `cpp/cmake-artifacts` | ~11-22 |
| 1.5 | `.sandbox\stabilization\{cmake-consumer,native-clean-cmake,napi-build}\Debug` | 9 d | `cpp/vcxproj-artifacts` | ~11-22 |
| 1.0 | `Assets3D\Toolbox\out` | 5 d | `cpp/cmake-artifacts` | ~11-26 |
| 0.4 | `UzEngine\napi\build`, `runtime\thirdparty\amd-fidelityfx-1.1\sdk\build`, demo builds | 9 d | `cpp/cmake-artifacts` | ~11-22 |
| **43.1** | | | | |

Not covered: `E:\HiveLLM\Tml\build` (6.8 GiB, idle 100 d) belongs to the TML compiler's own
build system (`tml.toml`, no `CMakeLists.txt` at the root; `build\llvm` is a CMake/Ninja tree
of LLVM). `E:\Falcor\build\windows-vs2022` (3.3 GiB, March) is a CMake tree, but Falcor's root
has no `CMakeLists.txt` and git does not ignore `build`. Both are owner decisions (08). A
generic "folder with `CMakeCache.txt`" rule would catch them; it needs an engine change (09).

## Other stacks (existing rules)

| Rule | Found on E: | Would clean now | Notes |
|---|---:|---:|---|
| `node/artifacts` (30 d) | 6.1 GiB in 26 `node_modules` | **3.9 GiB in 14** (HiveLLM Vectorizer 1.7, Nexus 0.96, Elyriu Dashboard 0.74, Options 0.19, TransmutationLite 0.15, Thunder, Umicp, Fluxum…) | the rest is UzEngine (idle 9 d) and HiveLLM Rulebook, Synap, Cortex (idle 14–18 d); pnpm stores in HiveLLM hard-link files between projects, so the freed bytes are lower than listed |
| `vcpkg/clone-build-files` (30 d) | 0.43 | 0.43 (`UzEngine\runtime\thirdparty\vcpkg\downloads` 0.39, `buildtrees` 0.04; idle 222 d) | |
| `rust/target` (7 d) | 0.06 | 0.06 (`HiveLLM\Synap\crates\synap-server\target`, `Nexus\crates\nexus-core\target`) | the big Synap target (41.75) is already in the Recycle Bin (04) |
| `dotnet/csproj-artifacts` (30 d) | 0.04 | 0.04 (`HiveLLM\Thunder\csharp\…Tests\bin`) | |
| `python/*-artifacts` (30 d) | 0.05 | 0 (`.mypy_cache` of UzPainter and Synap, idle 5 and 17 d) | |
| `unreal/project-artifacts` (ask, 14 d) | 0.14 (`Assets3D\Unreal`: `Intermediate`, `DerivedDataCache`) | 0 (the Assets3D repository is active) | ask |

Total of existing rules now: **~4.5 GiB** on E:. The roots of `data/workspaces.toml` include
E: since this task's coordinator changed them; before that, none of these ran here.

## Outside any rule (owner, 08)

| GiB | Folder | What it is |
|---:|---|---|
| 12.6 + 1.0 | the 8 agent worktrees after their caches and builds; `fix-vfx-pr7` | git worktrees Claude Code agents left. All 8 `worktree-agent-*` branches are merged into `main` (0 commits ahead); 6 are clean, 2 have 2 changed files each. 13 more `agent-*` folders are not registered worktrees any more (0.1 GiB together). `fix-vfx-pr7` is not merged. |
| 5.6 | `UzEngine\.sandbox\stabilization` after its caches and builds (13.75 in all) | git-ignored verification checkouts and logs of 2026-10-10 |
| 23.9 | `UzPainter\scripts\tmp` (104 run folders, 100,464 files, all of 2026-10-23) | pytest `tmp_path` trees (`test_*0`), snapshot and build copies of agent runs. UzPainter's AGENTS.override.md now forbids this folder, but also says to grep the task notes before deleting anything |
| 2.8 / 2.5 / 1.4 / 1.4 | `UzEngine\.screenshots`, `.rulebook` (memory index `memory.hnsw` 1.47 + `memory.db` 1.03, both of 2026-03-24), `cache` (engine shader/texture cache, git-ignored), `.tools` (pinned Zig 0.15.2, Node 22, their zips, npm cache) | project tooling and caches of the owner's own engine |
| 6.8 / 11.5 | `HiveLLM\Tml\build` (idle 100 d); Tml's `gcc` and `llvm-project` submodules (`.git\modules` 6.1, checkouts 5.4) | rebuilding LLVM takes hours, so not automatic |
| 5.0 | `HiveLLM\Expert\cli\venv_windows` (PyTorch, idle 197 d) | a virtual environment; never a default target (python pack) |
| 3.7 / 2.1 | `E:\Falcor` (NVIDIA Falcor clone of 2025-01 and its March build), `E:\godot` (Godot clone, `.git` 1.7, March) | third-party clones; re-clone when needed |
| 2.8 | `E:\tmp` (`norm-clippy-199` 2.0 is a Cargo `--target-dir` with `CACHEDIR.TAG`; `myclean-verify`, `android-setup`) | this week's agent scratch (newest 2026-10-31) |
