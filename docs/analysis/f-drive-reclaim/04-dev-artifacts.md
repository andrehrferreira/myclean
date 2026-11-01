# 04 Developer artifacts on F: (the project rules)

**Answer.** The 20 project-artifact rules would select **0 GiB on F: today**, and that is
right. The only artifacts inside git work trees are MyClean's own `build` folders (85.4 GiB),
in a repository that was committed to at 07:53 today. Every other artifact-looking folder
(`node_modules`, `dist`, `build`, `__pycache__`) is inside an installed program, outside any
git work tree, and the `git_ignored` guard keeps it. No threshold change is justified by F::
the oldest build configuration here is 29 hours old.

## Project roots found

A walk like `scan_artifacts` (F:\ to depth 8, skipping `.git`, `node_modules`, `.venv`, `venv`,
`$RECYCLE.BIN`, `System Volume Information`, `.myclean-quarantine`, and not entering artifact
folders) finds 440 marker folders:

| Marker (rule) | Where | Git work tree? |
|---|---|---|
| `CMakeLists.txt` (`cpp/cmake-artifacts`) | `F:\MyClean` and 118 subfolders; `F:\MyClean-deploy` and 118; `F:\Falcor`; 2 in `F:\Python313` | MyClean and MyClean-deploy (a worktree of it) yes; Falcor no (11 files, no `.git`); Python no |
| `package.json` (`node/artifacts`) | `F:\Node\auditoria-2turno-eleicao2026`; 102 in `F:\IDEs\Microsoft VS Code`; 74 in `F:\Python313`; 2 each in pgAdmin and PostgreSQL; 1 in `F:\Go` | only the audit project |
| `setup.py`, `pyproject.toml`, `requirements*.txt` (`python/*-artifacts`) | 14 + 3 + 1 inside `F:\Python313`, `F:\Python312`, pgAdmin, PostgreSQL | no |
| `*.vcxproj`, `*.sln`, `*.slnx`, `*.csproj`, `pubspec.yaml`, `build.gradle*`, `pom.xml`, `Cargo.toml`, `*.uproject`, `ProjectSettings\ProjectVersion.txt`, `.vcpkg-root` | none on F: | |

`F:\OndeSaiMaisBarato` (0.05 GiB) has no marker and no `.git`: documents and migrations only.

## Per rule

| Rule | Tier, guard | Candidates on F: | GiB | Selected | Why not |
|---|---|---|---:|---:|---|
| `cpp/cmake-artifacts` | auto, 30 days inactive, git-ignored | `F:\MyClean\build` | 76.71 | 0 | project active: last commit 2026-11-01 07:53, builds minutes ago |
| | | `F:\MyClean-deploy\build`, `out` | 8.57 + 0.11 | 0 | same repository, active (building now) |
| `node/artifacts` | auto, 30 days, git-ignored | 3 `node_modules`, 32 `dist`, 1 `build` in VS Code's install; `node_modules` in both pgAdmins | ~0.5 | 0 | not in a git work tree: nothing vouches they are regenerable (they are the programs) |
| | | the audit project | 0 | 0 | has no `node_modules`/`dist`/`build`/`coverage`/`.cache` |
| `python/setup-artifacts`, `pyproject-artifacts`, `requirements-artifacts` | auto, 30 days, git-ignored | 15 `__pycache__` in Python 3.12/3.13 and pgAdmin | < 0.05 | 0 | not in a git work tree (installed packages) |
| `cpp/vcxproj-artifacts`, `dotnet/*`, `flutter/artifacts`, `java-gradle/*`, `rust/target`, `rust/stale-incremental`, `unity/project-artifacts`, `unreal/project-artifacts`, `vcpkg/clone-build-files`, `visual-studio/*-state` | | no marker on F: | 0 | 0 | |

**On F: the project rules free 0 GiB.**

## MyClean's own build folders (85.4 GiB, not proposed)

`F:\MyClean\build` holds 63 build configurations, one or two (`-rel`) per agent or task. The
repository's history starts 2026-10-31 02:34. From each `.ninja_log`:

| Last built | Configurations | GiB |
|---|---|---:|
| 2026-11-01, 0–8 h ago (`debug`, `clean-progress`, `scanall-health`, `gov-svc`, `c-analysis`, `gov-core`, `gov-jobs`, `gov-games`) | 8 | 19.93 |
| 2026-10-31 03:23–13:56, 18–29 h ago (`a`…`e2`, `si`, `ti`, `s`, `wh`, `pl`, `cl`, `cmp`, `drv`, `map`, `packs`, `inbox`, `idx`, `exe`, `mig*`, `sa`, `release`, `sched`, `rules`, `store`, `safety`, `svc`, `telemetry`, each with its `-rel`) | 55 | 56.78 |

The 18–29 h ones belong to agents that no longer run (the active agents tonight are the eight
above plus `reclaim-*`, `wsl-tmp-code`, `main`). Per the brief they are dev artifacts of a
repo in active use: not proposed for deletion. The coordinator can remove a finished agent's
configuration (`build\<name>` and `build\<name>-rel`) when its work has merged; each frees
0.4–5 GiB.

`F:\MyClean-deploy\build` (debug 5.0, release 3.5, both built in the last 8 h) and `out\stage`
(0.11) are the deploy worktree's, in use.

## Should a rule reach stale configurations inside an active project?

The pattern seen here (many `build\<preset>` folders, each a complete CMake tree, most of them
idle while the project stays active) is what `rust/stale-incremental` solves for Cargo. A
CMake analogue would select `build\*` folders that hold a `CMakeCache.txt` and that no build
wrote for N days. Two reasons not to add it now:

- **Not expressible safely as data.** `targets = ['build\*']` with `min_age_days` would also
  select the *files and subfolders* of a single-configuration `build` folder: an idle
  `build\CMakeCache.txt` in an active build holds hand-set `-D` options (owner settings), and
  an idle `build\_deps` would force FetchContent to download again. The rule needs a condition
  "the selected folder contains `CMakeCache.txt`", which the engine lacks (a code change, 06).
- **No evidence for a threshold.** Every configuration on F: is under 30 hours old. A threshold
  needs folders idle for weeks; the D:/E:/G:/W: analyses may supply them.
