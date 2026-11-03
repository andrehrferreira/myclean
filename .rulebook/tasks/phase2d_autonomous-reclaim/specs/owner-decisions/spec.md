# Owner Decisions Specification

## ADDED Requirements

### Requirement: Owner Decisions Listed
The service SHALL list, through `space.owner_decisions` (read-only, on the tray's request, cached like the other space analyses), the cold folders of the owner's profile on an SSD (at least 1 GiB, nothing written for six months, scan-all's cold rule, outermost first) with the archive folder each would move to, and the installed programs whose Uninstall entry (HKLM 64-bit and 32-bit, the owner's HKCU) has an InstallLocation inside an area the knowledge packs mark for uninstall review, with their size (EstimatedSize, else the install folder's size on disk). Each entry SHALL carry a candidate id; a folder the deny-list protects SHALL be marked with the entry that refuses it, and what could not be judged SHALL be named in notes.

#### Scenario: A cold folder and a program in an uninstall area
Given the owner's profile on an NVMe drive holds `AppData\Local\OldStudio` (12 GiB, last written 16 months ago)
And the healthy HDD L: keeps more than 10 % free after taking 12 GiB
And AmpliTube 5 is installed in a folder of an area whose reclaim kind is uninstall
When the tray asks for the owner decisions
Then the list holds `C:\Users\<owner>\AppData\Local\OldStudio` with target `L:\Archive\C\Users\<owner>\AppData\Local\OldStudio`
And it holds AmpliTube 5 with its size and the area's note

#### Scenario: A protected folder is shown but cannot move
Given the owner's Documents folder has not changed for a year
When the owner decisions are listed
Then Documents is marked protected by `user_known_folder`
And the tray shows no button for it

#### Scenario: The MCP server only lists them
Given an MCP client calls `find_owner_decisions`
When the service answers
Then the client receives the list
And no MCP tool can create, approve or run an owner-decision plan

### Requirement: One Click Is The Approval
The tray SHALL show each owner decision with one button, "Mover para o HD" for a cold folder with a target and "Desinstalar" for a program; a click SHALL call the tray-only `plans.owner_action` with the candidate id, which creates exactly that one user-scope plan already approved by the owner, run by the tray with the owner's token. Any other caller SHALL be refused.

#### Scenario: Clicking Mover para o HD
Given the owner decisions list `OldStudio` with an archive target
When the owner clicks "Mover para o HD" on that row
Then an approved plan of kind `archive` exists for that folder alone
And the tray runs it on its next task poll

#### Scenario: Another caller cannot act
Given an MCP client knows the candidate id of a cold folder
When it calls `plans.owner_action` with that id
Then the service refuses with `refused`
And no plan is created

### Requirement: Move To HDD Archive
Archiving SHALL copy the folder with the tree copier to `<healthy HDD>\Archive\<letter>\<path>` (the archive folder on the HDD with the most room that keeps 10 % free, judged again when the plan is made and when it runs), SHALL verify that the copy holds the same files and bytes and that the folder still holds what was copied, and only then SHALL remove the folder through SafeRemove. It SHALL refuse, touching nothing, a folder that holds the current or image folder of a running process, a system location, a deny-list place, a folder replaced since the plan was made, a target that is not the folder's archive folder on another volume, and a target volume that is unhealthy or lacks room. Undo SHALL copy the archive back into the missing or empty folder, verify it, and remove the archive copy through SafeRemove.

#### Scenario: Nothing is deleted before a verified copy exists
Given the copy of `OldStudio` to L: loses one file
When the archive plan runs
Then the plan fails with "nothing was removed"
And `OldStudio` is unchanged
And no archive copy is left on L:

#### Scenario: A folder in use is refused
Given a running program's current folder is inside `OldStudio`
When the archive plan runs
Then it fails with `in_use` before anything is copied

#### Scenario: Undo brings the folder back
Given `OldStudio` was archived to L:
When the owner clicks Undo on the plan in Cleanup
Then `OldStudio` holds the same files and bytes as before
And the archive copy on L: is gone

### Requirement: Launch The Uninstaller
Uninstalling SHALL read the program's Uninstall entry again when the tray runs the plan, refuse when it is gone or names another program, and start its UninstallString (the program's own interactive uninstaller, with the UAC prompt when it needs elevation) with the owner's token, waiting a bounded time for it. The plan SHALL record what was launched; it SHALL be verified when the Uninstall entry is gone, and otherwise stay applied with "launched; finish in the uninstaller". It SHALL NOT be reversible. Tests SHALL never start a real uninstaller.

#### Scenario: Clicking Desinstalar
Given AmpliTube 5's UninstallString is `"C:\Program Files\AmpliTube 5\unins000.exe"`
When the owner clicks "Desinstalar" and the tray runs the plan
Then `unins000.exe` starts on the owner's desktop
And while AmpliTube 5 is still installed the plan says "launched ...; finish in the uninstaller"

#### Scenario: Uninstalled meanwhile
Given AmpliTube 5 was uninstalled after the list was made
When the owner clicks "Desinstalar"
Then the service refuses with `not_found` and no plan is created
