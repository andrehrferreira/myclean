# Proposal: phase4_optimize

## Why
Beyond cleaning and capping, the owner wants the whole machine optimized: startup, drive maintenance, indexing, leaks, boot time, background-app cost, and data placed on the right drive. Every optimization must be measured before and after and be reversible, never a generic tweak pack. Milestone M4 in docs/roadmap.md.

## What Changes
Startup inventory with measured impact and disable/enable; drive maintenance (re-TRIM, HDD defrag when idle); search-indexer scope; memory-leak detector; boot/logon tracking; background-app cost report; verify loop with auto-undo; live NTFS index ($MFT + USN) with growth tracking; photo near-duplicates; cold-data report; placement and Dev Drive advisors; WSL/Docker VHDX compaction; guided freeze elimination. Work packages O1–O7, S3–S5, P3, P4, C6, I3 and spikes X6–X7 in docs/dag.md.

## Impact
- Affected specs: optimizer, startup, ntfs-index, placement, input-watchdog
- Affected code: src/core/{analyze,advise}, src/platform/{ntfs,tasks,services,reg,disk}, src/tray, src/cli
- Breaking change: NO
- User benefit: Measured, reversible optimization of startup, drives, indexing and data placement; instant growth tracking
