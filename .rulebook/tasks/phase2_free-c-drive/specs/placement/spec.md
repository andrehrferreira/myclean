# Placement Specification

## ADDED Requirements

### Requirement: Relocation Preview
For TEMP/TMP and each supported package cache (Gradle, Cargo, npm, NuGet, pip, Go, Unreal DDC) the system SHALL show the current location, its size and a proposed target volume, and SHALL apply a relocation only after the owner approves it.

#### Scenario: Preview of the npm cache
Given the npm cache is at `C:\Users\<owner>\AppData\Local\npm-cache` and holds 1.4 GB
When the owner opens the Relocate view
Then the npm cache row shows that path, 1.4 GB and a proposed target volume other than C:
And no setting is changed until the owner approves the row

### Requirement: Relocation Through Supported Settings
The system SHALL relocate each item through its supported setting: the user variables `TEMP` and `TMP`, `GRADLE_USER_HOME`, `CARGO_HOME` together with a `PATH` fix for `.cargo\bin`, `npm config set cache`, `NUGET_PACKAGES`, `PIP_CACHE_DIR`, `GOMODCACHE` and `GOCACHE`, and Unreal `UE-LocalDataCachePath`, and MUST NOT use a directory junction when such a setting exists.

#### Scenario: TEMP relocated
Given the owner approves moving TEMP/TMP to `D:\Temp`
When the action applies
Then the user variables `TEMP` and `TMP` both equal `D:\Temp`
And no junction is created at the old TEMP path

#### Scenario: Cargo relocation keeps tools on PATH
Given the owner approves moving CARGO_HOME to `W:\cargo`
When the action applies
Then the user variable `CARGO_HOME` equals `W:\cargo`
And the user `PATH` entry for `.cargo\bin` points to `W:\cargo\bin`

### Requirement: Existing Content Handling
When a relocation applies, the system SHALL either move the existing content to the new location or leave it to regenerate, as chosen by the owner in the preview, and SHALL move files only through SafeRemove under the same plan.

#### Scenario: Let the cache regenerate
Given the owner chooses "regenerate" for the Gradle cache relocation
When the action applies
Then `GRADLE_USER_HOME` points to the new location
And the old cache content is offered as a separate cleaning item instead of being moved

#### Scenario: Move existing content
Given the owner chooses "move" for the NuGet cache relocation
When the action applies
Then the existing packages are present at the new location before `NUGET_PACKAGES` is switched

### Requirement: Exact Relocation Undo
The system SHALL store the previous value of every changed variable or tool setting, including its absence, and undo SHALL restore exactly that previous state.

#### Scenario: Undo restores a previous value
Given `TEMP` was `C:\Users\<owner>\AppData\Local\Temp` before relocation
When the owner undoes the relocation
Then `TEMP` equals `C:\Users\<owner>\AppData\Local\Temp` again

#### Scenario: Undo removes a variable that did not exist
Given `PIP_CACHE_DIR` was not set before relocation
When the owner undoes the relocation
Then `PIP_CACHE_DIR` is not set

#### Scenario: Undo restores PATH
Given a CARGO_HOME relocation changed the `.cargo\bin` PATH entry
When the owner undoes the relocation
Then the user `PATH` is identical to its value before the relocation

### Requirement: Healthy Relocation Target
The system MUST NOT propose or apply a relocation onto a drive whose health is degraded or unknown.

#### Scenario: Degraded drive excluded
Given the disk behind G: has a new media error
When relocation targets are proposed
Then G: is not offered as a target
And an approved relocation targeting G: is refused at apply time

### Requirement: System Space Settings Need Approval Unless the System Volume Is Critical
The system SHALL show the hibernation type, page-file placement and size, and shadow-storage size with the current value, the space it costs, a recommended value, the exact command it will run (`powercfg /h /type reduced`, WMI `Win32_PageFileSetting`, `vssadmin resize shadowstorage`) and its undo, and MUST change them only after an explicit owner approval, with two exceptions decided by the owner on 2026-11-01: while the system volume has less than 10 % free, the scheduler SHALL itself set full hibernation to reduced, and SHALL move a page file that is system-managed or uncapped on the system volume to a system-managed page file on the best healthy volume with room, keeping a small fixed page file on the system volume; it MUST NOT shrink the total page-file capacity the machine has, and MUST NOT change shadow storage. Each such change is a plan of the scheduler, recorded and undoable like an approved one, and the owner is told when a restart is needed.

#### Scenario: Hibernation entry
Given `C:\hiberfil.sys` is 50.9 GB with full hibernation
When the owner opens the Optimizations view
Then the hibernation entry shows 50.9 GB, the recommended "reduced" type, the command `powercfg /h /type reduced` and how to undo it

#### Scenario: Auto policy cannot change these settings while there is room
Given the system volume has 25 % free and the owner sets the placement feature's automation level to `auto`
When the scheduler runs
Then the hibernation type, page file and shadow storage are not changed
And any proposed change waits in the approval inbox

#### Scenario: Critical system volume
Given C: has 5 % free, full hibernation and a system-managed page file on C:, and D: is a healthy NVMe volume with room
When the scheduler runs
Then hibernation is set to reduced
And the page files become a small fixed one on C: and a system-managed one on D:, effective after a restart
And shadow storage is not changed
And both changes are plans of the scheduler that the owner can undo

#### Scenario: Page file undo
Given the owner approved moving the page file from C: to D:
When the owner undoes the action
Then the page-file settings are restored to their previous values, including system-managed sizing on C:
And the owner is told a restart is needed for it to take effect
