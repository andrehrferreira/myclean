# Proposal: phase2b_machine-wide-scan

## Why
The owner wants one scanner that covers all 8 drives (5 NVMe, 1 USB SSD, 2 HDD) and finds everything that can be removed, compressed, relocated, defragmented/trimmed or reorganized to free space and make the machine faster. C: alone has ~730 GB not explained by hiberfil/pagefile. Pulls S3, S5, P3 and the O2 analysis forward from phase4 and adds compression.

## What Changes
Volume map/explainer (myclean map); fast whole-volume index from the NTFS $MFT with USN updates (S1 fallback when not elevated); compression analyzer and reversible WOF/CompactOS actions; drive maintenance analysis (HDD fragmentation, SSD TRIM/optimization age) with defrag/retrim actions; cold-data and placement advisor across drives; cross-drive duplicates; one `myclean scan-all` orchestrator that produces a unified, categorized report (remove / compress / relocate / defrag-trim / deduplicate / archive / uninstall-review) with sizes and the exact action behind each finding.

## Impact
- Affected specs: map, ntfs-index, compression, drive-maintenance, scan-all
- Affected code: src/core/{map,index,compress,drives,scanall}, src/platform/{ntfs,compress,defrag}, src/cli, data/attribution.toml
- Breaking change: NO
- User benefit: One scan that shows every way to free space and speed up all drives, with the exact reversible action for each finding
