# Spec corrections from spikes X5, X6, X8 (apply when phases 3-5 start)

**Category**: spec-corrections
**Tags**: spikes, governor, startup, backup, spec-corrections

## Description

From docs/spikes/X5-games.md, X6-startup.md, X8-backups.md (2026-10-31):
GOVERNOR (phase3 Game Detection): do NOT use WindowsApps as a game root (Store Python/winget would be protected) — use <drive>:\XboxGames + MicrosoftGame.config; exclude Epic manifests whose AppCategories contain "engines" (UE editor/ShaderCompileWorker are class build); GameConfigStore match = Type 1 entry whose MatchedExeFullPath equals the image path (Type 2 = not games; name-only = weak); drop non-existent Steam libraries; seed owner roots from GameConfigStore paths outside launchers (no E:\Games exists); fullscreen scenario must cover borderless (QUNS_BUSY) and exclusive (pending owner manual test); notification-state query runs in the tray; read GameConfigStore via HKU\<SID>; GPU threshold uncalibrated.
STARTUP (phase4): StartupApproved = 12-byte binary, flags DWORD + FILETIME at offset 4; 02 enabled, 03/07 disabled, absent = enabled; read registry views explicitly; RunOnce has no StartupApproved; ignore orphan values; WHQL-signed services are third-party, list unsigned ones; HKLM writes need the service and x64 binaries with requestedExecutionLevel manifest (avoid UAC virtualization); undo stores all 12 bytes or "absent"; Diagnostics-Performance 100/101 need SYSTEM (go through the service; fallback Kernel-General 12, Winlogon).
BACKUP (phase5): OneDrive backup = User Shell Folders redirected under the account UserFolder + healthy account, no last-backup time; File History Config*.xml unverified (needs fixture); wbadmin/restore points/shadow copies run in the service, use exit codes not localized text; add manual dated copy folders (Backup-DD-MM-YYYY), *.dump + SHA256SUMS, *_gitlab_backup.tar, .bak as artifacts dated by folder CreationTime/name date; exclude .vhdx/.wim; add a manual-copy scenario.
