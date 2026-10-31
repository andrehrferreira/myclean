## 1. Implementation
- [x] 1.1 X3 spike: which IEmptyVolumeCache handlers run headless from a service and the sizes they report; note in docs/spikes/X3-cleanup-handlers.md
- [x] 1.2 S1: scanner v1 (FindFirstFileExW basic + large fetch, parallel per directory, no reparse traversal, size on disk, placeholders skipped)
- [x] 1.3 S2: space analytics: top-N files/folders per volume or path, old downloads (90 d, Zone.Identifier origin), installers already used (MSI ProductCode / EXE version info vs installed apps), exact duplicates (size -> 64 KB hash -> BLAKE3)
- [x] 1.4 C1: rule engine + catalog v1 as data (Windows temp/dumps/WER, browsers, Electron apps, IDEs, GPU shader caches, game launchers, package managers)
- [x] 1.5 C2: developer-artifact scanner over the configured workspace roots (default F:, W:) using marker files, git check-ignore, inactive-days and in-use checks
- [x] 1.6 C3: native prune commands (pnpm/npm/yarn/pip/uv/dotnet nuget/go/docker) run with the user token, Windows cleanup handlers and DISM StartComponentCleanup from the service
- [ ] 1.7 C4: cleaning executor: tiers auto/ask/report, permanent delete for auto, quarantine (same volume, or L: when the volume is < 10 % free) with 7-day purge and restore, Recycle Bin for report-view deletions, never onto a drive with degraded health
- [ ] 1.8 C5: recurring auto cleaning on the in-app scheduler with per-run manifest in the audit log
- [x] 1.9 P1: relocate TEMP/TMP and package caches (GRADLE_USER_HOME, CARGO_HOME + PATH, npm cache, NUGET_PACKAGES, PIP_CACHE_DIR, GOMODCACHE/GOCACHE, Unreal DDC) with exact undo
- [x] 1.10 P2: hibernation type, page-file placement/size and shadow-storage size as approval-only actions with cost, command and undo
- [ ] 1.11 R7: approval inbox in the tray: pending plans with items, bytes, risk and tier; approve all/some/none
- [ ] 1.12 R8: MCP proposal tools (propose_cleanup, propose_optimization) creating awaiting_approval plans; plus storage_top, find_old_downloads, find_duplicates, find_used_installers read tools
- [ ] 1.13 Acceptance: prompt 3 passes; C: free space measured before/after (target >= 25 % free); zero file operations outside a plan in the audit log; one week of unattended auto runs

## 2. Tail (docs + tests — check or waive with tailWaiver)
- [ ] 2.1 Update or create documentation covering the implementation
- [ ] 2.2 Write tests covering the new behavior
- [ ] 2.3 Run tests and confirm they pass
