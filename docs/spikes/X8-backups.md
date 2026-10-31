# X8 — Backup detection

- **Work package:** B1 (phase 5, `.rulebook/tasks/phase5_guard`)
- **Prompt:** 6, backups (`docs/analysis/myclean-planning/06-assistant-prompts.md`)
- **Spec:** `.rulebook/tasks/phase5_guard/specs/backup/spec.md`, requirements
  **Windows Backup Source Detection**, **Third-Party Tools And Backup Drive
  Artifacts**, **Last-Backup Age**, **Recommendation When No Backup Exists**
- **Machine:** Windows 11 Pro build 26200, pt-BR, run 2026-10-31,
  **non-elevated**, read-only. No backup setting was touched and no file
  content was read; only names, sizes and timestamps.

## Question

For each backup source, how do we detect it, where does it store copies, how do
we get the newest-copy time, and does it work without elevation?

## Summary table

| Source | Method | Non-elevated? | State on this machine |
|---|---|---|---|
| File History | `%LOCALAPPDATA%\Microsoft\Windows\FileHistory\Configuration\Config*.xml`; `fhsvc` | **Yes** (own profile) | **Never configured**: the `FileHistory` folder is absent, `fhsvc` is Manual/Stopped, HKCU `…\FileHistory` has only `RestoreUI`, no policy |
| OneDrive folder backup (KFM) | `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders` vs `HKCU\Software\Microsoft\OneDrive\Accounts\<acct>\UserFolder` | **Yes** | **Off**: Desktop/Documents/Pictures are `%USERPROFILE%\…`; OneDrive is in a sign-in error state and not running |
| Windows 7 "Backup and Restore" / system image schedule | `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsBackup` | Yes (key readable) | Key present but **empty**, so no schedule |
| System images (`wbadmin get versions`) | `wbadmin.exe` | **No**: exit code **-2**, localized pt-BR "Acesso negado … Administradores ou Operadores de Backup" | unknown non-elevated |
| System images (folders) | `<drive>:\WindowsImageBackup` on every volume | **Yes** (existence and top-level timestamps) | **None** on C D E F G K L W |
| Restore points | CIM `root/default:SystemRestore` | **No**: `CimException: Acesso negado` | unknown. `HKLM\…\Windows NT\CurrentVersion\SystemRestore` is readable (`SRInitDone=1`, `LastIndex=35`, `RPSessionInterval=1`, last maintenance 2026-10-29), but `…\SPP\Clients` is not readable |
| Shadow copies | CIM `Win32_ShadowCopy` / `Win32_ShadowStorage`; `vssadmin list shadows` | **No**: CIM "Falha de inicialização" (initialization failure, not "access denied"); vssadmin exit code **2** | unknown. `VSS` and `swprv` are Manual/Stopped, `SDRSVC` is Running |
| Third-party products | Uninstall keys (HKLM 64 + 32, HKCU) `DisplayName`; `Get-AppxPackage` | **Yes** | **None installed** (739 entries checked). Only Microsoft OneDrive matches a sync/backup pattern |
| Backup volumes K:/L: | top-level listing, then names-only peek and a bounded metadata walk | **Yes** (except `K:\Recovery` and `System Volume Information`) | **Manual dated copies** (see below). No `.mrimg`/`.vbk`/`.tibx`, `WindowsImageBackup` or `FileHistory` |

## Methods (exact)

```powershell
# File History
Test-Path "$env:LOCALAPPDATA\Microsoft\Windows\FileHistory"           # -> False
[Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser','Registry64').OpenSubKey('Software\Microsoft\Windows\CurrentVersion\FileHistory')
Get-Service fhsvc

# OneDrive / KFM
$a=[Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser','Registry64').OpenSubKey('Software\Microsoft\OneDrive\Accounts')
$a.GetSubKeyNames()                                                  # Business1, FileCoAuth, Personal
$u=[Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser','Registry64').OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders')
'Desktop','Personal','My Pictures' | % { $u.GetValue($_,$null,'DoNotExpandEnvironmentNames') }

# Elevation-gated sources
Get-CimInstance -Namespace root/default -ClassName SystemRestore
Get-CimInstance Win32_ShadowCopy; Get-CimInstance Win32_ShadowStorage
& C:\Windows\Sysnative\wbadmin.exe get versions; $LASTEXITCODE     # -2
& C:\Windows\Sysnative\vssadmin.exe list shadows; $LASTEXITCODE    # 2

# System image folders
foreach ($d in (Get-Volume | ? DriveLetter).DriveLetter) { Test-Path -LiteralPath "${d}:\WindowsImageBackup" }

# Installed products (DisplayName regex)
'Macrium|Reflect|Veeam|Acronis|True Image|Backblaze|Duplicati|Carbonite|Cobian|AOMEI|EaseUS|Paragon|Genie|Arq|Restic|Kopia|Duplicacy|FreeFileSync|SyncBack|GoodSync|Syncthing|Google Drive|Dropbox|iDrive|pCloud|MEGA|Synology|Seagate|WD Backup|Backup|Ghost|Iperius|UrBackup'

# Backup volumes: top level, then a bounded walk (depth 4, cap 50 000 entries), no reparse points followed,
# metadata only (DirectoryInfo.EnumerateFileSystemInfos; Attributes, CreationTimeUtc, LastWriteTimeUtc, Extension)
Get-ChildItem -LiteralPath 'K:\','L:\' -Force
```

## OneDrive values (observed)

`HKCU\Software\Microsoft\OneDrive\Accounts` has the subkeys `Personal`,
`Business1` (only 4 ECS/device-id values, with no `UserFolder` or
`UserEmail`) and `FileCoAuth`. `Personal` has 42 values. The relevant ones:

| Value | Kind | Observed |
|---|---|---|
| `UserFolder` | REG_SZ | the OneDrive root (redacted) |
| `UserEmail`, `cid` | REG_SZ | present (redacted; personal data, do not store) |
| `LastSignInResult` | DWORD | `-2147164635` = `0x8004DE25` (a failure HRESULT) |
| `SignInErrorStateStartTimestamp` | QWORD, **Unix seconds** | 2025-10-18 |
| `LastSignInTime` | QWORD, Unix seconds | 2022-12-19 |
| `LastAttemptedSignInTime` | QWORD, Unix seconds | 2026-10-31 |
| `LastKnownCloudFilesEnabled` | DWORD | 1 |
| `LastMigrationScanResult` | DWORD | 10 |

- **There is no value that names the protected folders.** No
  `KfmFoldersProtectedNow`-style value exists here, and the
  `HKLM\SOFTWARE\Policies\Microsoft\OneDrive` KFM policies are absent. The
  reliable KFM signal is the **known-folder redirection** itself: for each of
  Desktop (`Desktop`), Documents (`Personal`) and Pictures (`My Pictures`) in
  `User Shell Folders`, check whether it sits under the account's `UserFolder`.
  Here none do, so KFM is off. *The "on" state is not verified on this machine.*
- OneDrive health: not running, its HKCU Run entry is disabled through
  StartupApproved (`03`, see X6), and sign-in has been in an error state since
  2025-10-18. Even redirected folders would not be backed up, so the check should
  report the sign-in error, not only the redirection.
- There is no "last backup time" for OneDrive in the registry. Newest-copy time
  isn't available locally for KFM; report it as "continuous sync (time
  unknown)" when the account is healthy.
- The Windows 11 "Windows Backup" app uses the same OneDrive KFM for folders.
  No separate registry state was found (`HKCU\…\CurrentVersion\WindowsBackup` is
  absent).

## File History (observed)

Not configured, so the `Config1.xml`/`Config2.xml` **format, target and
last-backup values could not be verified here**. Other observations:
`fhsvc` is Manual/Stopped, and the task `\Microsoft\Windows\FileHistory\File History (maintenance mode)`
is Ready even with File History off, so the task's existence is not a signal.
The "configured" signal is the existence of
`%LOCALAPPDATA%\Microsoft\Windows\FileHistory\Configuration\Config1.xml`.
Its fields need a fixture from a machine with File History on.

## K: "Backup 1" and L: "Backup 2" (observed, names and timestamps only)

Both are 9.3 TB volumes. By size they are the two WDC WD101EFBX SATA HDDs in
`Get-PhysicalDisk`. K: has 5.1 TB free and L: 5.9 TB. No reparse points at
top level.

| Path | Folder created | Contents (names only / walk result) |
|---|---|---|
| `K:\Backup-17-04-2026` | 2026-04-17 17:46 | 2 project folders; walk **capped at 50 000** entries, 20 reparse points |
| `K:\Backup-11-09-2026` | 2026-09-11 07:01 | 4 project folders; walk **capped**, **492 reparse points** (`node_modules` links) |
| `K:\Backup-20-09-2026` | 2026-09-20 23:35 | 1 project folder, 11 entries, newest file created 2026-09-21 02:43 |
| `K:\Backup Git` | 2023-11-26 | `*_gitlab_backup.tar` ×2 (2023-05), `gitlab.rb`, `gitlab-secrets.json` |
| `L:\Backup-11-09-2026` | 2026-09-11 09:14 | 2 database folders, 119 entries, newest 2026-09-11 12:50 |
| `L:\Backup-24-09-2026` | 2026-09-24 23:24 | `ar_dashboard-prod-<UTC stamp>.dump` + `SHA256SUMS`, created 2026-09-25 02:40 |
| `L:\Database` | 2026-09-21 | one `.vhdx` (a database disk, modified 2026-10-09; **not** a backup image) |
| other K: folders | 2023–2026 | an owner-name folder, `Assets`, `UnityAssetsBackup`, `Videos`, project archives |
| `K:\Recovery` | 2026-09-30 08:51 | `UnauthorizedAccessException` non-elevated |

- The owner's real backups are **manual, dated copy folders**
  (`Backup-DD-MM-YYYY`), a **pg_dump file with checksums** and **GitLab backup
  tars**. None match the spec's artifact list (`.mrimg`, `.vbk`, `.tibx`,
  `WindowsImageBackup`, File History folders).
- Newest backup by folder date: `L:\Backup-24-09-2026` (2026-09-24/25), which
  is **about 36 days old** on 2026-10-31.
- Timestamps inside the copies are unreliable as "copy time". In
  `K:\Backup-11-09-2026` the newest file times seen (capped walk) were
  2026-03-27, older than the folder (2026-09-11), because the copy kept the
  source timestamps. The **folder's CreationTime** and the **date in its name**
  fell on the same day for all 5 `Backup-*` folders.
- A bounded walk of the two large copies took about 57 s for about 100 000
  entries on HDD. That is acceptable for a daily check, but too slow to run on
  every `check_backups` call.
- An extension list that includes `.vhdx`/`.wim` gives false positives
  (`L:\Database\ar-legacy.vhdx`).

## Conclusions

- Non-elevated: File History config, OneDrive/KFM state, the
  `WindowsImageBackup` folder scan, installed products and the volume scan all
  work. Restore points, shadow copies, `wbadmin` and `vssadmin` **need
  elevation**, so `check_backups` must get them from the SYSTEM service.
- `wbadmin`/`vssadmin` output is **localized** (pt-BR here). Use exit codes
  and WMI/VSS APIs, never text parsing.
- Under the current spec, this machine would be reported as **"no backup
  exists"**, although the owner made copies 36 days ago. That is a false alarm
  on the reference machine.

## Implications for the specs

1. **Windows Backup Source Detection** — OneDrive: define "folder backup on"
   as "known folder path under `Accounts\<acct>\UserFolder`" (from
   `User Shell Folders`), plus "account healthy" (no
   `SignInErrorStateStartTimestamp`/failed `LastSignInResult`). OneDrive
   timestamps are **Unix seconds**, not FILETIME. There is no newest-copy time
   for OneDrive, so the spec scenario's "each with … newest time" can't hold
   for OneDrive.
2. **Windows Backup Source Detection** — the File History `Config*.xml` values
   are **not verified** (not configured here). The spec says values "MUST be
   the ones verified in X8", so B1 needs a File History fixture from another
   machine or a VM before that part is built. Until then, detect only
   "configured / not configured" by the file's existence.
3. **Windows Backup Source Detection, "Unreadable source is reported as
   unknown"** — confirmed shape: `wbadmin` fails with exit code -2 non-elevated,
   CIM `SystemRestore` gives access denied, and `Win32_ShadowCopy` gives an
   *initialization failure* (a different error). Map all three to "could not be
   checked" and run them in the service. Use exit codes, not localized text.
4. **Third-Party Tools And Backup Drive Artifacts** — add **manual copy
   folders** (top-level folders on a backup volume whose name contains
   `backup` or a date) and **database/app dumps** (`*.dump` + `SHA256SUMS`,
   `*_gitlab_backup.tar`, `.bak`) as artifact kinds. Date them by **folder
   CreationTime / name date**, not by the newest file inside. Keep `.vhdx`/`.wim`
   **out** of the artifact list. The reparse-point rule is needed in practice:
   492 links in one copy.
5. **Last-Backup Age** — on the reference machine the correct answer today is
   "≈36 days, source: manual copy `L:\Backup-24-09-2026`". The scenario set
   should include a manual-copy case so prompt 6 doesn't wrongly report "no
   backup".
6. **Recommendation When No Backup Exists** — K: and L: are confirmed healthy
   candidates by size and free space (health itself not checked here). L: is
   also listed as a (stale) Steam library in `libraryfolders.vdf` (see X5), so
   "backup volume" must be discovered by label/role, not by "drive has no
   apps".
7. **Prompt 6 Answerable Through MCP** — the CLI (`--json`, non-elevated)
   can't produce restore points or shadow copies itself. It must ask the
   service, or label those sources "needs service".
