## 1. Implementation
- [ ] 1.1 X8 spike: File History, system image, OneDrive folder backup and third-party backup detection values; note in docs/spikes/X8-backups.md
- [ ] 1.2 E1: persistence points (Run keys, tasks, services, WMI event subscriptions, IFEO Debugger, Winlogon Shell/Userinit, AppInit_DLLs) and browser extensions for all Chromium profiles and Firefox with install origin (store, external, policy, unpacked)
- [ ] 1.3 E2: background network talkers over time from ETW Kernel-Network plus current TCP/UDP tables, reverse DNS
- [ ] 1.4 E3: verdicts from Defender (MSFT_MpThreatDetection, MpCmdRun file scan) and opt-in hash-only VirusTotal (off by default, API key required); anything else reported only as 'suspicious because'
- [ ] 1.5 A1: installed apps (Uninstall keys HKLM 64/32 + HKCU, Appx) with size, publisher, last used fused from UserAssist, Prefetch, BAM and MyClean's own process-start history, each with source and confidence
- [ ] 1.6 A2: classify apps removable / keep (OEM, runtime, driver, system Appx) / unknown; uninstall plans that launch the app's own uninstaller after tray approval
- [ ] 1.7 B1: backup detection (File History, system images, restore points, shadow copies, OneDrive backup, third-party tools, artifacts on K:/L:), last-backup age alert, git repos with no remote / unpushed / uncommitted work
- [ ] 1.8 MCP tools: scan_suspicious, list_browser_extensions, list_network_processes, list_installed_apps, propose_uninstall, check_backups
- [ ] 1.9 Acceptance: prompts 4, 5 and 6 pass in Claude Code; nothing removed or disabled without a tray approval

## 2. Tail (docs + tests — check or waive with tailWaiver)
- [ ] 2.1 Update or create documentation covering the implementation
- [ ] 2.2 Write tests covering the new behavior
- [ ] 2.3 Run tests and confirm they pass
