# Cleaning Specification

## ADDED Requirements

### Requirement: Rule Catalog as Data
The system SHALL load cleaning rules from data files covering Windows temp, caches and dumps, browsers, Electron apps, IDEs, GPU shader caches, game launchers and package managers, where each rule declares its targets, tier and strategy (delete, quarantine, native or handler), and MUST reject an invalid rule without applying any part of it.

#### Scenario: User temp rule
Given the built-in rule for the user `%TEMP%` folder
When the rule is evaluated
Then only files older than 2 days that are not locked by any process are selected

#### Scenario: Browser rule limits
Given a Chrome profile with HTTP cache, code cache, GPU cache, cookies and history
When the browser rule is evaluated without an explicit opt-in
Then only the HTTP cache, code cache and GPU cache are selected
And cookies, history, passwords and sessions are never selected

#### Scenario: Shader caches default to ask
Given the built-in rule for `%LOCALAPPDATA%\NVIDIA\DXCache`
When the catalog is loaded
Then the rule's tier is `ask`

#### Scenario: Invalid rule rejected
Given a rule file containing a rule with an unknown strategy value
When the catalog is loaded
Then that rule is rejected and reported
And no target of that rule is ever selected

### Requirement: Developer Artifact Qualification
The system SHALL find project roots by marker files under the configured workspace roots (default F: and W:) and SHALL propose an artifact folder only if all of the following hold: it matches the rule for that project type; inside a git repository `git check-ignore` reports it as ignored; no process has its current directory or an open file inside it; and the project has been inactive for at least the rule's inactive-days (default 14, 30 for `auto`-tier rules), where activity is the newest of the last commit and the newest source-file modification time.

#### Scenario: Ignored artifact of an inactive project
Given `F:\app\package.json` exists, `F:\app\node_modules` is ignored by git, no process uses it, and the last commit and newest source change are 45 days old
When the developer-artifact scanner runs
Then `F:\app\node_modules` is proposed with its size and rule

#### Scenario: Tracked folder never touched
Given `F:\lib\dist` matches the package.json rule but `git check-ignore` reports it as not ignored
When the developer-artifact scanner runs
Then `F:\lib\dist` is not proposed and none of its files are ever deleted

#### Scenario: Build running in the folder
Given a compiler process has an open file inside `F:\game\target`
When the developer-artifact scanner runs
Then `F:\game\target` is not proposed

#### Scenario: Active project
Given a project whose newest source file was modified 3 days ago
When the developer-artifact scanner runs with the default 14-day threshold
Then none of its artifact folders are proposed

#### Scenario: Unreal Saved folder
Given an Unreal project with `Saved\Logs`, `Saved\Crashes` and `Saved\Config`
When the developer-artifact scanner runs
Then only `Saved\Logs` and `Saved\Crashes` can be proposed and `Saved` as a whole is never proposed

### Requirement: Native Prune Commands Under the User Token
The system SHALL clean a global package cache with its tool's own command when one exists (`pnpm store prune`, `npm cache verify`, `yarn cache clean`, `pip cache purge`, `uv cache prune`, `dotnet nuget locals all --clear`, `go clean -cache -modcache`, `docker system prune`) and MUST run such commands in the tray process with the owner's user token, never as SYSTEM.

#### Scenario: pnpm store pruned natively
Given the pnpm store rule is approved for a run
When the plan executes
Then `pnpm store prune` is run by the tray process under the owner's token
And no file inside the pnpm store is deleted directly by MyClean

#### Scenario: Never as SYSTEM
Given the tray process is not running
When a plan containing a native prune command is executed
Then the service does not run the command
And the item is reported as not executed because the user context is unavailable

#### Scenario: Cache without a native command
Given the Gradle `caches` folder holds version folders 30 and 200 days old and the rule age is 90 days
When the rule is evaluated
Then only the 200-day-old version folder is selected

### Requirement: Windows System Areas Through Windows Handlers
The system SHALL clean Windows system areas only through the registered IEmptyVolumeCache handlers (Initialize, GetSpaceUsed, Purge) and `DISM /Online /Cleanup-Image /StartComponentCleanup`, run by the service, and MUST NOT delete files in those areas directly.

#### Scenario: Windows Update cleanup
Given the Windows Update Cleanup handler reports 7.2 GB reclaimable
When an approved plan runs that item
Then the service calls the handler's Purge
And no file under `C:\Windows\SoftwareDistribution` is deleted by MyClean's own delete path

#### Scenario: Handler unavailable headless
Given a handler that the X3 spike found cannot run without UI
When the catalog is loaded
Then that handler is not offered as a cleaning item

### Requirement: Cleaning Tiers
The system SHALL assign every cleaning item a tier: `auto` items run on schedule without a prompt, `ask` items are scanned on schedule and wait in the approval inbox, and `report` items are only shown and never acted on by MyClean; the owner MAY promote or demote any rule.

#### Scenario: Ask item waits
Given a scheduled scan finds 3 GB of shader caches with tier `ask`
When the scan completes
Then a plan in state `awaiting_approval` appears in the approval inbox
And no shader-cache file is removed until the owner approves

#### Scenario: Report item never acted on
Given a duplicate-file group with tier `report`
When any scheduled cleaning run executes
Then no file of that group is moved or deleted

#### Scenario: Owner demotes a rule
Given the owner changes the browser-cache rule from `auto` to `ask`
When the next scheduled run finds browser cache items
Then they wait in the approval inbox instead of being deleted

### Requirement: Removal Strategy per Tier
The system SHALL permanently delete `auto`-tier items and log a manifest (path, size, rule) for each run, SHALL quarantine `ask`-tier items, and SHALL send items the owner explicitly deletes from a report view to the Recycle Bin through IFileOperation with FOFX_RECYCLEONDELETE.

#### Scenario: Auto item deleted with manifest
Given an `auto` run removes 2 GB of crash dumps
When the run finishes
Then the files are permanently deleted
And the run manifest lists each path, size and rule

#### Scenario: Report-view deletion goes to the Recycle Bin
Given the owner selects one file of a duplicate group in the Space view and confirms delete
When the deletion executes
Then the file is in the Recycle Bin of its volume and can be restored with Windows' own undo

### Requirement: Quarantine and Restore
The system SHALL quarantine items into `<volume>\.myclean-quarantine\<run-id>\` on the item's own volume, or onto the configured quarantine volume (default L:) when the item's volume has less than 10 % free, SHALL purge quarantined items 7 days after quarantine, and SHALL restore every item to its original path from the manifest on request before purge.

#### Scenario: Same-volume quarantine
Given F: has 16 % free and an approved `ask` item lives on F:
When the plan executes
Then the item is renamed into `F:\.myclean-quarantine\<run-id>\`

#### Scenario: Overflow to L: when C: is critical
Given C: has 8.8 % free and an approved `ask` item lives on C:
When the plan executes
Then the item is moved to the quarantine folder on L:
And the space on C: is freed immediately

#### Scenario: Purge after 7 days
Given an item was quarantined 7 days ago and not restored
When the purge job runs
Then the item is permanently deleted and the purge is logged

#### Scenario: Restore
Given a quarantine run with 40 items that is 2 days old
When the owner restores the run
Then all 40 items are back at their original paths with their original names

### Requirement: Never Onto a Degraded Drive
The system MUST NOT quarantine or move any item onto a drive whose health is degraded or unknown.

#### Scenario: Overflow volume degraded
Given C: has less than 10 % free and the disk behind L: has a new reallocated sector
When an approved `ask` item on C: is executed
Then the item is not moved to L:
And the item is reported as skipped with the reason "quarantine volume health degraded"

### Requirement: Execution-Time Safety
Every cleaning removal SHALL go through SafeRemove, which MUST accept only items belonging to a stored plan, re-validate each item right before acting, skip locked files, and remove a junction or symlink as a link without touching its target.

#### Scenario: Junction removed as link
Given an approved artifact folder `node_modules` contains a junction to the pnpm store on another volume
When the folder is removed
Then the junction itself is removed
And every file in the pnpm store is untouched

#### Scenario: Locked file skipped
Given a `%TEMP%` file selected by an `auto` run is opened exclusively by a running process at execution time
When the run executes
Then the file is skipped and recorded as skipped in the manifest
And the run continues with the remaining items

#### Scenario: Item changed after approval
Given an approved item path was replaced by a junction to `C:\Users\<owner>\Documents` after the scan
When the plan executes
Then re-validation rejects the item and nothing under Documents is touched

#### Scenario: Item not in a plan
Given a request to remove a path that no stored plan contains
When SafeRemove is called
Then the removal is refused and logged

### Requirement: Recurring Auto Cleaning
The system SHALL run `auto`-tier cleaning on MyClean's in-app scheduler, with catch-up after downtime and without any OS scheduler, and SHALL write a per-run manifest to the audit log.

#### Scenario: Scheduled run
Given the auto-cleaning schedule is due
When the scheduler fires
Then `auto` rules are scanned and executed
And the audit log records the run with its manifest, bytes freed and skipped items

#### Scenario: Catch-up after downtime
Given the machine was off when a run was due
When the service starts
Then the missed run executes once

#### Scenario: No OS schedule
Given auto cleaning is enabled
When Task Scheduler is inspected
Then no MyClean task exists
