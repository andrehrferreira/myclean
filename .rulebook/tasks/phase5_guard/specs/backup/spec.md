# Backup Specification

## ADDED Requirements

### Requirement: Windows Backup Source Detection
The system SHALL check every Windows backup source and report, for each one, whether it exists, where it stores its copies and the timestamp of its newest copy: File History (`%LOCALAPPDATA%\Microsoft\Windows\FileHistory\Configuration`), system images (`wbadmin get versions` and `WindowsImageBackup` folders), restore points (WMI `root\default:SystemRestore`), shadow copies (`Win32_ShadowCopy`) and OneDrive folder backup (registry under `HKCU\Software\Microsoft\OneDrive\Accounts`). The OneDrive, File History and system image values MUST be the ones verified in the X8 spike note `docs/spikes/X8-backups.md`.

#### Scenario: Every source is reported, found or not
Given File History is configured to K:, no system image exists, and three restore points exist
When `check_backups` is called
Then the result lists File History (target K:, newest copy time), system image (none found), restore points (three, newest time), shadow copies and OneDrive folder backup, each with a found or not-found status

#### Scenario: Unreadable source is reported as unknown
Given `wbadmin get versions` fails because the service lacks access
When `check_backups` is called
Then the system image source is reported as "could not be checked" with the error, not as "none found"

### Requirement: Third-Party Tools And Backup Drive Artifacts
The system SHALL detect installed third-party backup products (for example Macrium, Veeam Agent, Acronis, Backblaze, Duplicati) and run a bounded-depth scan of the backup volumes (K: "Backup 1" and L: "Backup 2" on the reference machine, discovered at runtime rather than hard-coded) for backup artifacts (`.mrimg`, `.vbk`, `.tibx`, `WindowsImageBackup`, File History folders), reporting each artifact's location and newest timestamp. The scan MUST NOT follow reparse points or hydrate cloud placeholders.

#### Scenario: Macrium image on L:
Given Macrium Reflect is installed and `L:\Images\` holds `.mrimg` files, the newest dated 2026-10-12
When `check_backups` is called
Then the result lists Macrium Reflect as installed and its artifacts on L: with newest timestamp 2026-10-12

#### Scenario: Junction on a backup drive is not followed
Given `K:\Old` is a junction pointing to `C:\`
When the backup drives are scanned
Then the scan does not descend into `K:\Old`

### Requirement: Last-Backup Age
The system SHALL report the last-backup age as the time since the newest copy among File History, system images, OneDrive folder backup, third-party tools and artifacts on the backup volumes, naming the source of that newest copy. Restore points and shadow copies MUST be reported but MUST NOT count toward the last-backup age, because they live on the same volume as the data they protect.

#### Scenario: Age names its source
Given the newest File History copy is 3 days old and the newest Macrium image is 19 days old
When `check_backups` is called
Then the last-backup age is 3 days with source "File History"

#### Scenario: Restore points alone are not a backup
Given the only copies on the machine are restore points from yesterday
When `check_backups` is called
Then the restore points are listed and the result states that no backup exists

### Requirement: Backup Age Alert
The system SHALL raise an alert through the alert engine (toast and tray badge, rate-limited, snoozable) when the newest backup is older than N days, where N is set by the owner.

#### Scenario: Stale backup raises one alert
Given the owner set N to 7 and the newest backup is 9 days old
When the daily backup check runs
Then one alert "newest backup is 9 days old (File History)" is shown as a toast and on the tray badge

#### Scenario: Fresh backup raises nothing
Given N is 7 and the newest backup is 2 days old
When the daily backup check runs
Then no backup alert is raised

#### Scenario: Snoozed alert stays quiet
Given the backup-age alert was snoozed for 3 days
When the daily backup check runs on the next day and the backup is still stale
Then no toast is shown for that alert

### Requirement: Repository Safety Check
The system SHALL find the git repositories under the configured workspace roots (F: and W: on the reference machine, plus any root the owner confirms) and report each repository that has no remote, has local commits not present on any remote-tracking ref, or has uncommitted changes. The check MUST be read-only and local: it MUST NOT fetch, push, commit, or modify any repository.

#### Scenario: Three kinds of unprotected work
Given repository A has no remote, repository B has 4 commits ahead of `origin/main`, and repository C has modified files that are not committed
When `check_backups` is called
Then A is reported as "no remote", B as "4 unpushed commits", and C as "uncommitted changes", each with its path

#### Scenario: Check makes no network call
Given a repository has a remote on GitHub
When the repository check runs
Then no `git fetch` or other network operation is performed and the comparison uses the local remote-tracking refs

### Requirement: Recommendation When No Backup Exists
The system SHALL, when no backup is found, give a short concrete recommendation that uses the owner's own hardware (for example File History to K: and a monthly system image to L:), naming only drives that exist and whose health is not degraded. The system MUST only explain the setup and MUST NOT configure File History, system images or any backup tool itself.

#### Scenario: Recommendation without action
Given no backup source has any copy and K: and L: are healthy HDDs with more than 5 TB free
When `check_backups` is called
Then the result contains a recommendation naming K: and L: with the steps to set them up, and no plan or action is created

#### Scenario: Degraded drive is not recommended
Given L: reports a new reallocated sector and its health is degraded
When a backup recommendation is produced
Then L: is not named as a backup target and the reason is stated

### Requirement: Prompt 6 Answerable Through MCP
The system SHALL expose `check_backups` as an MCP read tool, with the same data available from the CLI with `--json`, so that prompt 6 (backups) can be answered with all sources checked, the last-backup age, unprotected repositories and, when needed, a concrete recommendation.

#### Scenario: Prompt 6 against fixture state
Given a fixture machine state with File History off, a 40-day-old Macrium image on K:, and one repository with unpushed commits
When prompt 6 is pasted into Claude Code with `myclean mcp` registered
Then the answer covers every backup source, states a last-backup age of 40 days with source Macrium, lists the repository, and the audit log shows no action
