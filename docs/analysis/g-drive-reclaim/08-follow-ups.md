# 08 Follow-ups for code (not done here)

1. **`require.outside_git` for tool-owned paths** (06-E). Engine: `qualify()` in
   `src/core/clean/engine.cpp` (the `not_in_git` drop, twice), parser
   (`src/core/rules/parse_toml.cpp`, `Requirements`), the spec sentence and scenario,
   `catalog_test` (`ArtifactRulesRequireEveryQualification` keeps `git_ignored` required and
   allows `outside_git` only on rules that never target a hand-made name such as `build`,
   `dist`, `out`, `bin`, `obj`, `Binaries`), the fixture runner (nothing: a project without
   `.git` is already "outside git"). Unlocks 33.77 GiB auto and 32.03 GiB ask on G:.
2. **`myclean map` does not use project rules.** G: is 0.00 % explained although 114.7 GiB are
   folders the project rules know. The map could run `scan_artifacts` over the workspace roots
   (without the git and inactivity checks, which are about cleaning, not naming) and attribute
   each artifact folder to its rule, so `G:\ToS\Client\Intermediate` shows as Unreal build
   output instead of "unexplained".
3. **`?:\` in clean targets is silently ignored.** `packs::parse` does not check clean targets
   for `?:`, and `RuleEngine` resolves `?:\` as a literal root, so such a rule loads and never
   matches. Either reject it at load (`must name a drive or a %VARIABLE%`, like `path_exists`) or
   expand it to every fixed drive.
4. **Cold-data granularity.** On G: the outermost cold folder is `G:\ToS` (1,032 GiB) because
   only `G:\Substance` changed in the last 6 months. A move proposal that size mixes backups
   (good on an HDD) with project trees (bad on an HDD). The cold report could stop at folders
   named like backups or archives (`_Backups`, `*Backup*`, folders that are mostly `.zip`/`.rar`)
   or offer the largest cold children instead.
5. **Archives already extracted.** No finding kind says "this archive's members exist extracted
   on the volume". The member listing is cheap (zip central directory, rar headers); a
   manual-review finding with the matched share would surface 75.6 GiB on G: (04).
6. **The `map` binary that produced `map-G.json` rejected the `wsl-docker` pack**
   (`'wsl tmp prune' is not a native command MyClean runs`): it predates the native command.
   Rebuild before the next map run so the pack loads.
