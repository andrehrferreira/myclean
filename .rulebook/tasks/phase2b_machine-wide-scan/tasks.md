## 1. Implementation
- [ ] 1.1 MAP: data/attribution.toml + core/map explainer + `myclean map <volume|path>` (categories, reclaim kind, unexplained and not-visible bytes)
- [ ] 1.2 X7 spike + S3: $MFT reader (FSCTL_ENUM_USN_DATA / raw MFT) and USN-journal updates per NTFS volume, elevated; S1 fallback non-elevated; consistent with a full rescan
- [ ] 1.3 CMP: compression analyzer (sampled compressibility, exclusions: compressed media, VHDX, databases, files in use or recently written, Windows dir) + WOF (XPRESS/LZX) and CompactOS actions with exact undo (decompress)
- [x] 1.4 DRV: drive maintenance analysis (HDD fragmentation via retrieval pointers sampling, free-space fragmentation, SSD last optimize/retrim age, media type) + defrag/retrim actions run by the service when idle and healthy
- [ ] 1.5 COLD/PLACE: cold-data finder (large folders untouched for N months on NVMe -> HDD archive candidates) and placement advisor (projects on QLC/USB, games on USB, data on a degraded drive -> move to a healthy drive)
- [ ] 1.6 DUP: duplicate finder across all volumes (size -> head hash -> full hash, hard links excluded), keep-one suggestions
- [ ] 1.7 SCANALL: `myclean scan-all` orchestrator over every fixed volume, below-normal priority, unified categorized report (remove / compress / relocate / defrag-trim / deduplicate / archive / uninstall-review) with bytes, drive, confidence and the plan/action behind each finding; JSON for MCP/tray
- [ ] 1.8 Acceptance: scan-all runs on all 8 drives of the reference machine and explains >= 95 % of used bytes on C: (rest listed as not visible without admin)

## 2. Tail (docs + tests — check or waive with tailWaiver)
- [ ] 2.1 Update or create documentation covering the implementation
- [ ] 2.2 Write tests covering the new behavior
- [ ] 2.3 Run tests and confirm they pass
