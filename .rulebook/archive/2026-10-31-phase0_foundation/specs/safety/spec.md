# Safety Specification

## ADDED Requirements

### Requirement: Single Removal Primitive
`SafeRemove` MUST be the only code path that deletes or moves files or folders; every cleaning, quarantine, restore and relocation action SHALL call it.

#### Scenario: Actions remove only through SafeRemove
Given the `DeleteItems` and `QuarantineItems` actions running against the fake file system
When they apply a plan
Then every delete or move reaches the file system through `SafeRemove`
And the fake file system records no delete or move from any other caller

### Requirement: Hard Deny-List
`SafeRemove` MUST refuse, regardless of which rule or plan requested it, any path that is a volume root; under `%WINDIR%` except explicitly allow-listed subpaths such as `%WINDIR%\Temp`; under any `Program Files*` folder; a `.git` folder or anything inside one; the owner's Documents, Desktop, Pictures, Videos or Music folders; another user's profile; or MyClean's own data or install folders. Rule files SHALL NOT be able to change the deny-list.

#### Scenario: Volume root refused
Given a plan item whose path is `D:\`
When `SafeRemove` is asked to delete it
Then the item is refused with reason `deny_list:volume_root` and nothing is deleted

#### Scenario: Windows folder refused, allow-listed subpath accepted
Given plan items `C:\Windows\System32\drivers\etc\hosts` and `C:\Windows\Temp\setup.log`
When `SafeRemove` processes both
Then the first is refused with reason `deny_list:windir`
And the second is accepted

#### Scenario: Program Files refused
Given plan items under `C:\Program Files\App` and `C:\Program Files (x86)\App`
When `SafeRemove` processes them
Then both are refused with reason `deny_list:program_files`

#### Scenario: Git folder content refused
Given a plan item `F:\repo\.git\objects\pack\pack-1.pack`
When `SafeRemove` processes it
Then it is refused with reason `deny_list:git`

#### Scenario: User known folders refused
Given plan items inside the owner's Documents, Desktop, Pictures, Videos and Music folders
When `SafeRemove` is asked to delete or quarantine them
Then each is refused with reason `deny_list:user_known_folder`

#### Scenario: Other user's profile refused
Given a plan item under `C:\Users\OtherUser\AppData\Local\Temp`
When `SafeRemove` processes it on behalf of the owner
Then it is refused with reason `deny_list:other_profile`

#### Scenario: MyClean data refused
Given plan items `C:\ProgramData\MyClean\myclean.db` and `C:\Program Files\MyClean\current\bin\myclean-svc.exe`
When `SafeRemove` processes them
Then both are refused

#### Scenario: Rule file cannot widen access
Given a rule file whose pattern matches `C:\Program Files\Vendor\cache`
When a plan built from that rule reaches `SafeRemove`
Then the item is refused with reason `deny_list:program_files`

### Requirement: Path Normalization Before Matching
`SafeRemove` MUST normalize every path (case, `.` and `..` segments, `\\?\` prefixes, 8.3 short names, trailing dots and spaces) before evaluating the deny-list, so that no spelling of a denied path is accepted.

#### Scenario: Traversal spelling refused
Given a plan item `C:\Users\owner\AppData\Local\Temp\..\..\..\Documents\a.docx`
When `SafeRemove` processes it
Then the path is normalized to the owner's Documents folder and refused

#### Scenario: Short-name spelling refused
Given a plan item `C:\PROGRA~1\App\app.exe`
When `SafeRemove` processes it
Then the path is normalized to `C:\Program Files\App\app.exe` and refused

### Requirement: Never Follow Reparse Points
`SafeRemove` MUST open entries without following reparse points, MUST NOT descend into a junction, symbolic link or other reparse point during recursive removal, and SHALL remove such an entry as a link only, leaving its target untouched.

#### Scenario: Junction removed as a link
Given a folder `node_modules\pkg` that is a junction to `F:\store\pkg` containing files
When `SafeRemove` deletes the `node_modules` folder recursively
Then the junction itself is removed
And every file under `F:\store\pkg` still exists

#### Scenario: Directory symlink to a denied path
Given a cache folder containing a directory symbolic link that points to `C:\Windows\System32`
When `SafeRemove` deletes the cache folder recursively
Then only the link is removed and nothing under `C:\Windows\System32` is touched

### Requirement: Never Hydrate Cloud Files
`SafeRemove` MUST skip, without reading their content, files carrying `FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS`, `FILE_ATTRIBUTE_RECALL_ON_OPEN` or `FILE_ATTRIBUTE_OFFLINE`, and SHALL report sizes as size on disk.

#### Scenario: Cloud-only placeholder skipped
Given a plan item that is a OneDrive cloud-only placeholder with `RECALL_ON_DATA_ACCESS`
When `SafeRemove` processes it
Then the item is skipped with reason `cloud_placeholder`
And no download of its content is triggered

#### Scenario: Placeholder inside a folder being removed
Given a folder containing one local file and one `RECALL_ON_OPEN` placeholder
When `SafeRemove` deletes the folder recursively
Then the local file is removed, the placeholder is skipped and the folder is kept

### Requirement: Plan Membership
`SafeRemove` MUST accept only items that belong to a plan MyClean itself created and stored, addressed by plan id and item id; it SHALL NOT accept a raw path from any caller.

#### Scenario: Item not in plan refused
Given stored plan 42 with items 1 and 2
When `SafeRemove` is asked to act on plan 42 item 3
Then the request is refused with reason `not_in_plan` and nothing is touched

#### Scenario: Raw path refused
Given a request that carries a file path but no stored plan item
When it reaches `SafeRemove`
Then it is refused and the refusal is written to the audit log

### Requirement: Re-Validation Before Acting
`SafeRemove` MUST re-validate each item immediately before acting on it: path normalization, deny-list, reparse and cloud attributes, and that the file's identity (volume serial and file id) still matches what the plan recorded.

#### Scenario: TOCTOU swap to a junction
Given a plan item recorded as the regular folder `F:\proj\target`
And after the plan was stored that folder was replaced by a junction to the owner's Documents folder
When `SafeRemove` acts on the item
Then the item is refused with reason `changed_since_plan`
And nothing under Documents is touched

#### Scenario: File replaced by a different file
Given a plan item whose file was deleted and recreated at the same path with a different file id
When `SafeRemove` acts on the item
Then the item is refused with reason `changed_since_plan`

### Requirement: Per-Item Outcome
`SafeRemove` MUST return a per-item outcome (removed, moved, skipped or refused, with the reason and the bytes freed) and SHALL continue with the remaining items when one item is refused or skipped.

#### Scenario: Mixed plan
Given a plan with one valid temp file, one deny-listed path and one cloud placeholder
When `SafeRemove` runs the plan
Then the temp file is removed, the denied path is refused and the placeholder is skipped
And three outcomes are returned, with bytes freed counted only for the removed file

### Requirement: One Test per Guard
The test suite MUST contain at least one test per deny-list entry, plus tests for a junction, a symbolic link, a cloud placeholder, a TOCTOU swap and an item not in the plan.

#### Scenario: Deny-list entry without a test
Given a deny-list entry is added to `SafeRemove`
When the test suite runs
Then a coverage test that enumerates the deny-list entries fails until a test for the new entry exists
