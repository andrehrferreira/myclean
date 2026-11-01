# 03 Developer artifacts on G: (the project rules)

**Answer.** G: holds **114.7 GiB** in folders the project rules name (build output, IDE
state, `node_modules`, `target`). Today the rules select **30.7 GiB auto** (28.3 of it the Rust
`target` the coordinator already counted) and **22.6 GiB ask** (Unreal output and `.vs` of the
two backups that are in git). The new `visual-studio/intellisense-cache` rule (06) turns 11.1
GiB of that ask into auto. **61.2 GiB are not selected because their projects are not in git**
(`G:\ToS\Client`, `NoSafePlaces`, `UnderTheShadow`, `UE_Projects\*`,
`GenericServer\ClientNetworkSample` and two backups): 33.8 in `.vs` and 27.4 in Unreal output.
Every project rule requires a git work tree that ignores the folder. Inactivity never blocks
anything: nothing on G: was written after 2026-04-17.

## Project roots found

A walk like `scan_artifacts` (G:\ to depth 8, skipping `.git`, `node_modules`, `.venv`, `venv`,
`$RECYCLE.BIN`, `System Volume Information`, `.myclean-quarantine`, not entering artifact
folders) finds:

| Marker | Roots | In a git work tree |
|---|---|---|
| `*.uproject` | 31: `ToS\Client`, `NoSafePlaces\Client`, `NoSafePlaces\AssetsNormalization`, `UnderTheShadow`, 6 in `UE_Projects`, `GenericServer\ClientNetworkSample`, 11 creature projects in `ToS\Assets - Custom\Creatures\*\Unreal`, 7 in `_Backups`, `FlopperamUnrealMCP` in a plugin | only `_Backups\Backup - ToS3\2025\Client` (repo `2025`) and `2024\AssetsNormalize` (own repo) |
| `*.sln` / `*.slnx` | 58 / 0 (the Unreal projects' `.sln`, servers, SteamSDK sample) | the servers and the two backups above |
| `*.csproj` | 22 outside artifact folders | servers (`ToS-Server-5`, `GenericServer\Server`, backups) |
| `Cargo.toml` | `ToS\Server` (workspace) and 3 members, `tools\test-framework\rust-load-test`, `ToS-Server-5\Benchmark`, `Server - LastCSharp\Benchmark` | all |
| `package.json` | `Tools\heightmap-generator`, `Tools\spell-icon`, `Server\tools\test-framework`, `ToS-Server-5` and `\Api`, `Server - LastCSharp` and `\Api`, `Server - LastTs`, `2024\Dashboard`, 25 inside `repo-recuperado` (Unity packages) | all but `spell-icon` and `Server - LastTs` |
| `CMakeLists.txt` | `Server\tools\client-simulator` | yes |
| `ProjectSettings\ProjectVersion.txt` | `2023\repo-recuperado` (Unity) | yes (no `Library`: never opened) |
| `setup.py`, `pyproject.toml`, `requirements*.txt` | 26, inside `Intermediate\PipInstall` and plugin `Content\Python` | never reached (inside artifact folders) or nothing to select |
| `*.vcxproj` | 68, nearly all in `Intermediate\ProjectFiles` (not entered); `SteamSDk\steamworksexample` | no |

Git repositories on G: (last commit): `ToS\Server` (2025-11-28), `ToS\Server\ToS-Server-5`
(2025-08-28), `ToS\Tools\heightmap-generator` (2025-11-27), `ToS\Plugins\AINiagara` and three
nested (2025-11-15), `GenericServer\Server` (2025-09-05), and in `_Backups`: `Server - LastCSharp`
(2025-08-28), `Backup - ToS3\2025` (2025-04-11), `2025\Server` (2024-10-28), `2024\ServerCSharp1.0`
(2024-08-17), `2024\Dashboard` (2024-03-11), `2024\AssetsNormalize` (2025-01-31), `2023` (no
commit), `2023\repo-recuperado` (2021-11-27), `Backup - ToS1\Server3` (2023-04-04), two plugins in
`NetworkSample\Plugins`. Three `libsodium` folders have a `.git` file (a submodule) whose
module folder is missing; git fails there.

## Per rule

| Rule | Tier, guard | Candidates on G: | GiB | Selected | Why the rest is not |
|---|---|---|---:|---:|---|
| `rust/target` | auto, 7 days, git-ignored | `ToS\Server\target` (newest 2025-11-30) | 28.32 (26.83 on disk) | 28.32 | (already handled by the 7-day change) |
| | | `_Backups\Server - LastCSharp\Benchmark\target` | 0.52 | 0.52 | |
| `rust/stale-incremental` | auto, 7 days | inside the two `target` folders | (in the above) | (in the above) | `target` goes whole |
| `node/artifacts` | auto, 30 days, git-ignored | 4 `node_modules` in git (`Server - LastCSharp\Api` 0.92, `heightmap-generator` 0.24, `2024\Dashboard` 0.16, `Server\tools\test-framework` 0.13) | 1.46 | 1.46 | `Tools\spell-icon\node_modules` 0.03: not in git |
| `dotnet/sln-artifacts`, `csproj-artifacts` | auto, 30 days, git-ignored | `Server3\GameServer\bin` 0.22, `Server3\Tests\bin` 0.03, `ServerCSharp1.0\bin` 0.12, `Server - LastCSharp\bin` 0.01 | 0.37 | 0.37 | `2025\FastUDP\bin`/`obj` and the other UDP projects: git tracks files in them (`tracked_files`) |
| `cpp/cmake-artifacts` | auto, 30 days | `client-simulator\build` 0.0004, `build_test` 0.0004 | 0.00 | `build` | `build_test` is not ignored and its `CMakeLists.txt` is tracked |
| `cpp/vcxproj-artifacts` | auto, 30 days | `SteamSDk\steamworksexample` | 0.00 | 0 | not in git; nothing built |
| `unreal/project-artifacts` | **ask**, 14 days, git-ignored | `2025\Client` (`Intermediate` 5.69, `Binaries` 0.50, `DerivedDataCache` 0.22, `Saved\Logs` 0.10, `Saved\Crashes` 0.11), `2024\AssetsNormalize` (`Intermediate` 4.72, DDC 0.03, `Saved\Crashes` 0.01) | 11.39 | 11.39 (ask) | |
| | | 29 projects not in git: `ToS\Client\Intermediate` 10.31, `NetworkSample` 4.50, `UnderTheShadow` 2.51, `PlayerController` (backup) 2.47, `ClientNetworkSample` 2.31, `MutableSample\DerivedDataCache` 1.50 (`Compressed.ddp`), `ClothTest55` 1.20, creature projects 1.21, others 1.4 (Binaries 1.48 of the total) | 27.40 | 0 | `not_in_git` |
| `visual-studio/sln-state` | **ask**, 14 days, git-ignored | 15 `.vs` folders, 45.06 GiB | 45.06 | 11.18 (ask: `2025\Client` 10.54, `Server - LastCSharp` 0.31 + 0.05, `2025\Server` 0.16, `ServerCSharp1.0` 0.04, `GenericServer\Server` 0.02 + 0.05, `Server3` 0.01) | 33.83: not in git (`NetworkSample` 17.29, `NoSafePlaces\Client` 3.42, `UnderTheShadow` 3.41, `ToS\Client` 3.37, `PlayerController` 3.31, `ClientNetworkSample` 3.04); `ToS\Client\Plugins\ToS_Network\…\libsodium\.vs` 0.05: `git_error` |
| `unity/project-artifacts` | ask, 14 days | `repo-recuperado` | 0 | 0 | no `Library`, `Temp`, `Obj` or `Logs` |
| `python/*`, `flutter`, `java-gradle/*`, `vcpkg/clone-build-files`, `cpp` (vcxproj) | | no candidate with content | 0 | 0 | |

Totals: auto 30.67 GiB (28.32 already counted by the coordinator), ask 22.57 GiB (11.39
Unreal + 11.18 `.vs`).

## What the guards miss on G: (and what it is)

| GiB | What | Where | Regenerable? |
|---:|---|---|---|
| 44.74 | Visual Studio's IntelliSense precompiled-header cache (`.vs\<sln>\v17\ipch`, 17.76), browse databases (`Browse.VC.db`, `Solution.VC.db`, 12.65), Copilot's code index (`CopilotIndices`, 12.55), Find-in-Files index (`FileContentIndex`, 1.78) | all 15 `.vs` folders; the remaining 0.32 is `.suo`, `DocumentLayout*.json`, `fileList.bin`, `*.projects.v*.bin` (per-user state) | yes: VS rebuilds them when the solution opens (no build), see 06 |
| 25.92 | Unreal `Intermediate`, `DerivedDataCache`, `Saved\Logs`, `Saved\Crashes` | projects not in git | yes: UnrealBuildTool and the editor recreate them; a C++ project recompiles its modules |
| 4.82 | Unreal `Saved\Cooked` 2.61, `Saved\StagedBuilds` 2.21 | `ClothTest55`, `NetworkSample` | yes: cook and stage output; no rule names them yet |
| 1.58 | Unreal `Saved\webcache_4430` / `webcache_6613` (the editor's embedded Chromium cache) | 9 projects | yes; no rule names them yet |
| 1.98 + 2.21 | project `Binaries`, packaged `Build\Windows` | not in git | only with sources; owner |
| 13.45 | `Saved\Autosaves`, `Saved\Backup` | 9.25 in `2025\Client` | recovery copies; owner |
| 2.80 | `MutableSample\Saved\MutableStreamedDataEditor` | | probably (Mutable recompiles); unverified, owner |

No change of `inactive_days` is justified by G:: the youngest project (`Assets - Custom\Creatures\Mushroom\Unreal`)
was last written 2026-04-17, 198 days ago. The gap is git, not time. 06 proposes the new
cache rule (in spec, data only) and the `outside_git` guard (a small code and spec change)
that would let the two rules above reach the projects outside git, with every other guard
kept.
