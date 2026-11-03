# Proposal: phase2_free-c-drive

## Why
C: has 84 GB free of 952 GB while every other drive has hundreds of GB to TBs free; temp, Windows caches, developer caches, the hibernation file and the page file keep refilling it. The owner needs safe recurring cleaning plus relocation of what regrows, with nothing deleted without approval except regenerable data. Milestone M2 in docs/roadmap.md; evidence F-001..F-018.

## What Changes
Scanner v1 and space analytics (top-N, old downloads, used installers, exact duplicates); rule engine and catalog; developer-artifact cleaning with git-ignored/inactive/in-use checks; native prune commands, Windows cleanup handlers and DISM; cleaning executor with auto/ask/report tiers, quarantine and restore; recurring auto cleaning; relocation of TEMP/TMP and package caches; hibernation/page-file/shadow-storage options; approval inbox and MCP proposal tools. Work packages S1, S2, C1–C5, P1, P2, R7, R8 and spike X3 in docs/dag.md.

## Impact
- Affected specs: space, cleaning, placement, approvals
- Affected code: src/core/{rules,advise,safety}, src/platform/{fs,msi,shell,reg}, src/tray (inbox), src/cli (mcp tools), data/rules
- Breaking change: NO
- User benefit: C: stops being strangled; recurring safe cleanup; regrowth sources moved off C:; prompt 3 answerable
