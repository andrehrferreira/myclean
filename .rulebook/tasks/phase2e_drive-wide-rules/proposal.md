# Proposal: phase2e_drive-wide-rules

## Why
The E:, F: and G: analyses (docs/analysis/{e,f,g}-drive-reclaim) found ~250 GiB MyClean could clean on its own but cannot: clean targets cannot name other drives (?:\ is silently dropped), pure caches in projects outside git are never offered, Zig and Visual Studio project caches have no rule, and the map explains almost none of the workspace drives.

## What Changes
Engine and pack changes so every 100 %-safe finding of the three analyses becomes an auto rule, plus the follow-ups that need code (map attribution of workspace projects, marker-folder targets, recycle-bin age, extracted-archive and idle-worktree findings, cold-data granularity).

## Impact
- Affected specs: cleaning spec (Developer Artifact Qualification, drive wildcards)
- Affected code: src/core/clean, src/core/rules, src/core/packs, src/core/map, src/core/scanall, data/packs
- Breaking change: NO
- User benefit: ~250 GiB more reclaimed on its own across E:, F:, G:; the map explains the workspace drives
