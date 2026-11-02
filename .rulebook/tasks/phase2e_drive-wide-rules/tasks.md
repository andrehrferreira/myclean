## 1. Implementation
- [x] 1.1 Clean targets accept ?:\ as every fixed volume; the pack parser rejects a target the engine would drop (today a ?:\ target loads and matches nothing) — F 06, G 08, E 09
- [x] 1.2 require.outside_git: a project rule may opt in to projects outside any git work tree (inside one, git must still vouch); spec sentence, parse_toml, qualify(), catalog_test — G 06-E
- [x] 1.3 visual-studio/intellisense-cache and slnx-intellisense-cache (auto: ipch, Browse/Solution.VC.db and -wal/-shm, CopilotIndices, FileContentIndex; 7 idle days) with outside_git — G 06-A (44.7 GiB on G:)
- [x] 1.4 zig/project-cache (auto: .zig-cache and zig-cache of build.zig projects, 7 idle days) and its attribution area; zig 1.2.0 — E 07 (172 GiB on E:)
- [x] 1.5 python/pip-uninstall-leftovers (auto, site-packages\~* of 7 days, needs 1.1) and python/installations widened to ?:\Python3? — F 06
- [x] 1.6 unreal/project-artifacts gains Saved\Cooked, Saved\StagedBuilds, Saved\webcache_* (ask); Binaries split into a git-only project-binaries case — G 06-B
- [x] 1.7 Attribution areas: wsl-docker/docker-data-custom, dev-tools/sdks ?:\LLVM, new databases pack (mongodb, postgresql, pgadmin), unreal/megascans-library, windows/installer-rollback — F 06, G 06-C/D
- [ ] 1.8 games/steam-library-shadercache via a Steam-library variable read from libraryfolders.vdf (core/games parses it); confirm with a Valve source that Steam rebuilds a removed pre-cache — E 07
- [x] 1.9 Rules acting inside active projects (inactive_days 0, e.g. rust/stale-incremental) count only a process inside the item itself as using it, not one anywhere in the project (Smoke run: 0 selected, 1,286 dropped in_use by idle terminals)
- [x] 1.10 myclean map attributes the artifact folders the project rules find on workspace roots (E: 569 GiB, G: 115 GiB unexplained today) — E 09, G 08; done as map::project_areas over the scan itself (myclean map and scan-all)
- [x] 1.11 A rule can target the folder holding its marker (CMakeCache.txt, CACHEDIR.TAG): idle build configurations inside active projects — F 04, E 09; done as require.contains and cpp/stale-build-tree (auto, 14 days)
- [ ] 1.12 windows/recycle-bin: items deleted more than 30 days ago (deletion time from the $I record) as an auto case — E 04
- [ ] 1.13 Findings: archive already extracted (G: 75.6 GiB), merged clean idle git worktree (E: 13.7 GiB) — G 08, E 09
- [ ] 1.14 Cold-data report proposes folders below a mixed top-level tree (G:\ToS mixes backups and live projects) — G 08
- [ ] 1.15 Docker: a data-disk folder that is not CustomWslDistroDir becomes an ask case — F 07
- [x] 1.16 Review the shipped zig/global-cache per-entry deletion and cpp/cmake-artifacts matching files such as build.zig — E open risks; zig/global-cache no longer deletes o entries one by one (h, z, tmp only); cpp/cmake-artifacts build* still relies on git_ignored for files such as build.zig
- [ ] 1.17 Measure the hourly artifact scan over D:, E:, F:, G:, W: and bound it if it is heavy (a read-only walk of those roots at depth 8 took 86 min in the debug ASan build)
- [x] 1.19 The service runs git as the owner (owner session token and environment): as SYSTEM git refused every owner repository, so no rule needing git ever proposed anything (0 rust/target items in the first deployed scans)
- [ ] 1.18 Tail: docs/packs.md, the cleaning spec, CHANGELOG, full quality gate, deploy

## 2. Tail (docs + tests — check or waive with tailWaiver)
- [ ] 2.1 Update or create documentation covering the implementation
- [ ] 2.2 Write tests covering the new behavior
- [ ] 2.3 Run tests and confirm they pass
