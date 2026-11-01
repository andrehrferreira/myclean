# 09 Follow-ups that need code (not done here)

1. **Clean targets on every drive.** A clean target must name a real drive or a `%VARIABLE%`:
   the clean engine drops `?:\…` (`target_entries` in `src/core/clean/engine.cpp`, "a target must
   be an absolute path"), while attribution and relocation accept `?:\`. So no shipped rule can
   reach a launcher's library on a second drive. Two ways, the second is better:
   - expand `?:\` in clean targets to each local fixed drive (as the map does for areas), with
     the same deny-lists;
   - a `%STEAM_LIBRARY%` target variable with one value per library of `libraryfolders.vdf`
     (`src/core/games/steam.cpp` already parses it for the governor). Needed by
     `games/steam-library-shadercache` (07); 0.17 GiB here, more on machines with big libraries.
2. **The map does not explain workspace projects.** 569 GiB of E:'s 791 (72 %) is "unexplained"
   only because the map has no area for project folders on a workspace drive: attribution paths
   are absolute patterns, `*` within one component, no "any depth". The map could run the
   workspace scanner (`RuleEngine::scan_artifacts` over `data/workspaces.toml`) and label each
   artifact it finds `build-artifacts` with its rule as `clean-rule`, and each project root
   `dev-projects`. Then the depth-by-depth `zig/project-cache` area (07) can go, and Rust
   `target`, `node_modules` and CMake `build` folders on D:, E:, F:, G: and W: are explained
   too.
3. **Recycle Bin by deletion age.** An auto case "items deleted more than 30 days ago" needs the
   deletion time from each `$I` record (offset 16, FILETIME) and the `$R` item beside it; the
   engine compares only file times. Storage Sense's setting does the same thing; MyClean could
   offer to turn it on as a setting instead.
4. **A target that is the marker's folder.** A CMake build tree is recognisable by
   `CMakeCache.txt` inside it, and any cache directory by `CACHEDIR.TAG`
   (`E:\tmp\norm-clippy-199`, Cargo; pytest and mypy caches write it too). Rules cannot say "the
   folder holding the marker": targets are relative to the marker's folder and cannot be `.`.
   This would cover `E:\Falcor\build\windows-vs2022` (3.3), `HiveLLM\Tml\build\llvm` (1.4) and
   Cargo `--target-dir`s outside projects (2.0).
5. **Stale merged worktrees.** 76 GiB on E: are git worktrees left by Claude Code agents, all
   merged into `main`. A finding "worktree whose branch is merged and whose tree is clean,
   untouched for N days" with the action `git worktree remove` would be safe and reversible
   (the commits stay); today it can only be an owner decision.
6. **`zig/global-cache` deletes `o`, `h` and `z` entries one by one** (30 days). A manifest in
   `h` that outlives its output in `o` can make Zig reuse a missing file. The Zig docs only
   promise that deleting the whole cache is harmless. Either prove (test) that Zig recovers
   from a missing `o` entry, or change the rule to remove whole cache trees only when nothing
   in them was written for 30 days. Not observed to fail; a risk, not a bug report.
7. **`cpp/cmake-artifacts` targets `build*`**, which also names files such as `build.zig`,
   `build.zig.zon`, `build.ps1` in a project that has a `CMakeLists.txt` (UzEngine's `runtime`
   has both). Only the `git_ignored` guard keeps them; restricting the targets to folders would
   make the rule safe on its own.
8. **Folders the owner cannot list.** 48 folders under `E:\UzmiGames\UzPainter` (`tmp\scratch\…\pytest`,
   `scripts\tmp\codex-audit\…`, `.claude\worktrees\…\.pytest_cache`) deny the owner's token
   (Win32 error 5). Likely made by another account (a Codex sandbox user, or Python 3.13's
   `mkdir(mode=0o700)`, which on Windows gives the creator alone access); an assumption, not
   checked. Their size is inside the 4.0 GiB not visible. User-scope cleaning cannot remove them;
   a `scan-all` finding "folders in a workspace you cannot read" would tell the owner.
9. **Two numbers to read with care**: the map counts allocated bytes and each hard link once
   (HiveLLM's pnpm stores hard-link the same file into several `node_modules`); a plain walk of
   E: counts 805 GiB instead of 791 because it follows junctions (`Elyriu\Dashboard\node_modules\client`
   → `..\client`) and counts every link. The rule emulation in 02 used logical sizes, so its
   `node_modules` figure is an upper bound.
