# Cleaning Specification

## ADDED Requirements

### Requirement: Rule Catalog as Data
The system SHALL load cleaning rules from data files covering Windows temp, caches and dumps, browsers, Electron apps, IDEs, GPU shader caches, game launchers and package managers, where each rule declares its targets, tier and strategy (delete, quarantine, native or handler), and MUST reject an invalid rule without applying any part of it. A target is an absolute path under a `%VARIABLE%`, a drive or `?:\` (the same path on every local fixed volume), or, for a project rule, a path relative to the project root; a target that could never match (relative without a project, absolute with one, or with a `..` component) makes the rule invalid. A few variables are read from a tool's own files and stand for several folders: `%STEAM_LIBRARY%` is every library that Steam's `libraryfolders.vdf` lists, and a target naming it is resolved in each; when the file cannot be read or parsed it has no value and the target selects nothing. Steam's shader pre-cache in every library (`steamapps\shadercache\<app id>`) is a GPU shader cache, `auto` once nothing was written in it for 7 days (Valve's Steam client notes: Steam downloads pre-compiled shaders for the video card, and deletes them itself when the driver or graphics card changes; without them a game compiles its shaders, with some stutter at first). A rule MAY require that a selected folder hold a file named by a path relative to it (`contains`) and MAY name folders it must keep (`outside_of`): a folder that lies in or holds one of them is never selected, and when that path names a variable without a value the rule selects nothing. `%DOCKER_WSL_DATA%` is every folder Docker Desktop's settings name for its data (`CustomWslDistroDir` of `settings-store.json` and of the legacy `settings.json`, as the owner's `%APPDATA%` holds them) and its default `%LOCALAPPDATA%\Docker\wsl`; when a settings file that exists cannot be read or parsed, or neither exists, it has no value. A Docker Desktop data folder (`DockerDesktopWSL*`, holding `disk\docker_data.vhdx`) at most two levels below a fixed volume's root that no setting names, unchanged for 30 days and with no file open, is a copy Docker never reads: it is offered as an `ask` case, never `auto` (it may hold the only copy of a volume).

#### Scenario: User temp rule
Given the built-in rule for the user `%TEMP%` folder
When the rule is evaluated
Then only files older than 2 days that are not locked by any process are selected

#### Scenario: Browser rule limits
Given a Chrome profile with HTTP cache, code cache, GPU cache, cookies and history
When the browser rule is evaluated without an explicit opt-in
Then only the HTTP cache, code cache and GPU cache are selected
And cookies, history, passwords and sessions are never selected

#### Scenario: Shader caches are auto
Given the built-in rule for `%LOCALAPPDATA%\NVIDIA\DXCache`
When the catalog is loaded
Then the rule's tier is `auto` (the driver rebuilds a shader cache; the cost is a one-time stutter, never lost work)
And a cache a running game holds open is skipped

#### Scenario: Any-drive target
Given a rule targeting `?:\Python3*\Lib\site-packages\~*` and fixed volumes C: and F:, and a USB stick H:
When the rule is evaluated
Then `F:\Python313\Lib\site-packages\~orch` can be selected and nothing on H: is

#### Scenario: Every Steam library
Given Steam's `libraryfolders.vdf` lists `C:\Program Files (x86)\Steam` and `E:\SteamLibrary`, `E:\SteamLibrary\steamapps\shadercache\275850` was last written 29 days ago and `shadercache\365670` 2 days ago, and `W:\OldSteam\steamapps\shadercache\440` is in no listed library
When the rule targeting `%STEAM_LIBRARY%\steamapps\shadercache\*` is evaluated
Then `E:\SteamLibrary\steamapps\shadercache\275850` is selected (auto), and neither `365670`, the pre-cache under Program Files (deny-list) nor anything on W: is
And when `libraryfolders.vdf` cannot be read, nothing is selected

#### Scenario: Docker data folder left by a move
Given Docker Desktop's `settings-store.json` names `D:\Database\DockerDesktopWSL` and the legacy `settings.json` names `F:\WSL\wsl\DockerDesktopWSL`, Docker has not run for 40 days, and `F:\WSL\wsl\DockerDesktopWSL.ANTIGO\disk\docker_data.vhdx` was last written 50 days ago and is not open
When the rule `wsl-docker/docker-data-copy` is evaluated
Then `F:\WSL\wsl\DockerDesktopWSL.ANTIGO` is proposed as an `ask` item, and `D:\Database\DockerDesktopWSL` is not
And when `settings-store.json` cannot be read, nothing is proposed

#### Scenario: Invalid rule rejected
Given a rule file containing a rule with an unknown strategy value
When the catalog is loaded
Then that rule is rejected and reported
And no target of that rule is ever selected

### Requirement: Developer Artifact Qualification
The system SHALL find project roots by marker files under the configured workspace roots (default D:, E:, F:, G: and W:) and SHALL propose an artifact folder only if all of the following hold: it matches the rule for that project type; inside a git repository `git check-ignore` reports it as ignored; no process has its current directory or an open file inside it; and the project has been inactive for at least the rule's inactive-days (default 14, 30 for `auto`-tier rules), where activity is the newest of the last commit and the newest source-file modification time. An `auto`-tier rule MUST wait at least 7 days, either for the project (inactive-days) or for the artifact itself (min-age-days, the newest write inside it). A Cargo project's target goes after 7 inactive days (what `cargo clean` removes; the next build compiles again), and the incremental crate folders no build wrote for 7 days go even while the project is active; for such a rule (inactive-days 0) only a process with its current directory or an open file inside the item itself keeps it, while a process folder anywhere in the project keeps every other artifact of it. A Zig project's `.zig-cache` goes after 7 inactive days (the Zig build-system guide: it can be deleted at any time; the next build compiles again). A rule MAY require that a selected folder hold one named file (`contains`); a CMake build tree, recognised by its `CMakeCache.txt`, that no build wrote for 14 days goes even while its project is active (removing a build tree is how CMake starts over; the next configure recreates it), while a build subfolder without that file, such as FetchContent's `_deps`, is never selected by that rule. An `auto`-tier rule whose targets are pure caches written only by a tool, never a folder a person names by hand (Visual Studio's IntelliSense, browse-database, Copilot and search caches inside `.vs`), MAY set `outside_git`: a project outside any git repository is then offered on the other conditions, with the cache itself unchanged for at least 7 days; inside a repository git must still report it ignored and tracking nothing. The service MUST run git as the owner (the token and environment of the owner's logon session), never as SYSTEM: git refuses a repository owned by another user, and a repository's own configuration must not run with the service's rights. While the owner is not logged on, nothing inside a repository is proposed.

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

#### Scenario: Stale incremental cache of an active Cargo project
Given `F:\engine\src\lib.rs` was modified yesterday, `F:\engine\target` is ignored by git and no process uses the project, and `target\debug\incremental` holds `engine-a` last written 12 days ago and `engine-b` written yesterday
When the developer-artifact scanner runs
Then only `F:\engine\target\debug\incremental\engine-a` is proposed, and `target` as a whole is not

#### Scenario: Idle terminal in a project
Given a terminal has had `F:\engine\src` as its current directory for days, `F:\engine\target` is ignored by git, and `target\debug\incremental\engine-a` was last written 30 days ago
When the developer-artifact scanner runs
Then `target\debug\incremental\engine-a` is proposed, and `F:\engine\target` as a whole is not (in use)

#### Scenario: Tool cache of a solution outside git
Given `G:\ToS\Client\Client.sln` is in no git repository, its newest source change is 200 days old, no process uses it, and `.vs\Client\v17\ipch` was last written 200 days ago next to `.vs\Client\v17\.suo` and `bin`
When the developer-artifact scanner runs
Then `.vs\Client\v17\ipch` is proposed (auto), and `.suo` and `bin` are not

#### Scenario: Idle build tree of an active CMake project
Given `F:\engine\src\main.cpp` was modified yesterday, `F:\engine\build` is ignored by git, `build\asan` holds `CMakeCache.txt` and was last written 20 days ago, `build\debug` holds `CMakeCache.txt` written yesterday, and `build\_deps` holds no `CMakeCache.txt`
When the developer-artifact scanner runs
Then only `F:\engine\build\asan` is proposed (auto)

#### Scenario: The service asks git as the owner
Given the service runs as SYSTEM, `D:\V3\dashboard` is a git repository owned by the owner, its `target` is ignored by git and the project has been inactive for 10 days
When the developer-artifact scanner runs in the service
Then git runs with the owner's token and `D:\V3\dashboard\target` is proposed (auto); with the owner logged off nothing in the repository is proposed

#### Scenario: Unreal Saved folder
Given an Unreal project with `Saved\Logs`, `Saved\Crashes` and `Saved\Config`
When the developer-artifact scanner runs
Then only `Saved\Logs` and `Saved\Crashes` can be proposed and `Saved` as a whole is never proposed

### Requirement: Native Prune Commands Under the User Token
The system SHALL clean a global package cache with its tool's own command when one exists (`pnpm store prune`, `npm cache verify`, `yarn cache clean`, `pip cache purge`, `uv cache prune`, `dotnet nuget locals all --clear`, `go clean -cache -modcache`, `docker system prune`) and MUST run such commands in the tray process with the owner's user token, never as SYSTEM. A package manager's own prune command only removes what the tool downloads or rebuilds again, so its rule is tier `auto`; `docker system prune`, which also removes stopped containers that may hold the owner's state, stays `ask`.

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

#### Scenario: Handlers of regenerable data are auto
Given the handlers Thumbnail Cache, Internet Cache Files, Delivery Optimization Files, Temporary Setup Files and Upgrade Discarded Files, which remove only caches and setup leftovers
When the catalog is loaded
Then their rules are tier `auto`
And every other handler (crash dumps, error reports, update and driver rollback, previous installations, reset images, logs, the Recycle Bin) stays `ask`

#### Scenario: Handler unavailable headless
Given a handler that the X3 spike found cannot run without UI
When the catalog is loaded
Then that handler is not offered as a cleaning item

### Requirement: Aged Recycle Bin Items
The system SHALL treat a `Recycle Bin` handler case with `require.min_age_days` as the owner's Recycle Bin items deleted at least that many days ago on every fixed volume, judged by the deletion time each item's `$I` record holds and never by file times. The service SHALL size the plan from the owner's `$I` records (items whose `$R` content is gone count nothing) and propose it as one user-scope `recycle_bin` plan; the tray SHALL delete those items permanently through the shell's Recycle Bin folder with the owner's token and no UI. The deny-list keeps `?:\$Recycle.Bin` out of MyClean's own delete path, and an age below 7 days MUST NOT be proposed or run. The whole bin stays the `ask` handler case.

#### Scenario: Only items deleted long enough ago
Given the owner's bin on E: holds items deleted 31, 30 and 12 days ago, and one deleted 140 days ago whose content Windows already removed
When the scan evaluates `windows/recycle-bin-aged` (auto, 30 days)
Then one approved user-scope plan is sized by the items deleted 31 and 30 days ago
And the tray deletes only the items the shell lists as deleted 30 days ago or earlier

#### Scenario: Never through the delete path
Given an approved aged Recycle Bin plan
When it runs
Then the tray deletes each item with IFileOperation, FOF_NO_UI and no FOFX_RECYCLEONDELETE, on a private desktop
And the plan is recorded as not reversible and the tray offers no undo

#### Scenario: Too young an age
Given a case asking for items deleted 3 days ago
When the scan evaluates it
Then no plan is proposed and the scan reports a problem
And the tray refuses such a task without reading the bin

### Requirement: Cleaning Tiers
The system SHALL assign every cleaning item a tier: `auto` items run on schedule without a prompt, `ask` items are scanned on schedule and wait in the approval inbox, and `report` items are only shown and never acted on by MyClean; the owner MAY promote or demote any rule.

#### Scenario: Ask item waits
Given a scheduled scan finds 3 GB of JetBrains old-version folders with tier `ask`
When the scan completes
Then a plan in state `awaiting_approval` appears in the approval inbox
And no file of those folders is removed until the owner approves

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
