# NTFS Index Specification

## ADDED Requirements

### Requirement: Index Built From the MFT
The system SHALL build the index of an NTFS volume by reading its `$MFT` in one sequential pass through a read-only volume handle, recording for every file and folder its names (hard links), parent folders, attributes and sizes, and MUST NOT open, read or hydrate the indexed files' contents, nor send any request that writes to the volume, creates a change journal or deletes one.

#### Scenario: Initial build of C:
Given the service runs elevated and C: is an NTFS volume with no index yet
When the service builds the index of C:
Then every folder on C: has a total size on disk in the index
And no indexed file was opened and no cloud placeholder was hydrated

#### Scenario: Fragmented $MFT
Given the runs of the `$MFT` continue in an extension record named by the attribute list of record 0
When the index is built
Then the records in every run of the `$MFT` are indexed

#### Scenario: Damaged record
Given one FILE record of the `$MFT` fails its update sequence check
When the index is built
Then that record is skipped and the build completes

### Requirement: Scanner v1 Fallback
The system MUST refuse to open a volume for the index when the process is not elevated (access denied) or the volume is not NTFS (invalid argument), and SHALL then produce the folder tree with scanner v1 instead.

#### Scenario: Not elevated
Given the process is not elevated
When the folder tree of C: is requested
Then opening `\\.\C:` is refused with access denied
And scanner v1 lists C: instead

#### Scenario: Indexed volume answers without a walk
Given C: is indexed
When the folder tree of `C:\Users` is requested
Then it comes from the index without a directory listing

### Requirement: Same Folder Tree as Scanner v1
The index SHALL return the tree of its root or of any of its folders in the scanner v1 shape (`space::ScanResult`) with the totals a scanner v1 scan of the same folder gives: sizes on disk are allocated clusters (a compressed or sparse stream counts the clusters it occupies, a Windows-compressed file counts its `WofCompressedData` stream), a reparse point that names another entry or any reparse folder is a link of zero size that is never followed, a cloud placeholder is flagged and counts its local part only, and a file with several hard links is counted once, at the link that comes first in a depth-first walk by name. The index MUST additionally show the NTFS metadata files of the root (`$MFT`, `$LogFile`, `$Bitmap`, ...).

#### Scenario: Hard-linked file counted once
Given a file with the links `C:\Users\alice\copy.bin`, `C:\Users\alice\shared.bin` and `C:\Windows\System32\shared.bin`
When the tree of C: is taken from the index
Then the file's size is counted at `C:\Users\alice\copy.bin` only
And the tree of `C:\Windows\System32` alone counts it at `shared.bin`

#### Scenario: Junction not followed
Given `C:\Users\alice\Link` is a junction
When the tree is taken from the index
Then `Link` is listed as a link of zero size and nothing below its target is counted under it

#### Scenario: Same totals as a scan
Given the same volume content
When the index tree and a scanner v1 scan of `C:\` are compared
Then every folder has the same size on disk, size, file count and folder count

### Requirement: Compact Index
The in-memory index MUST stay below 100 bytes per file name: one fixed-size node per name, the UTF-8 name in a shared pool and one slot per `$MFT` record.

#### Scenario: 100 000 files
Given a volume with 1 000 folders of 100 files whose names are about 20 characters long
When the index is built
Then it holds less than 100 bytes per name

### Requirement: Index Kept Current by the USN Journal
The system SHALL keep the index current by reading the volume's USN change journal (`FSCTL_READ_USN_JOURNAL`) from the position noted before the `$MFT` was read: a deleted file is removed, every other file named in the journal is re-read from its FILE record and re-linked, a folder keeps its contents and moves its totals with it, and a folder created in the same batch is read before its contents. The updated index MUST equal an index read again from the `$MFT`.

#### Scenario: File written
Given the index of F: is current
When a build writes 3 GB into `F:\game\target`
Then the totals of `F:\game\target` and of each folder above it grow by 3 GB without reading the `$MFT` again

#### Scenario: Folder moved
Given `C:\Users\alice\OneDrive` holds 40 MB
When it is moved to `C:\Windows\Cloud`
Then `C:\Users` shrinks and `C:\Windows` grows by 40 MB and the files are listed under `C:\Windows\Cloud`

#### Scenario: Record reused
Given `notes.txt` was deleted and its FILE record now holds a new file
When the journal is applied
Then `notes.txt` is gone and the new file is listed in its folder

### Requirement: Rebuild When the Journal Cannot Cover the Gap
The system SHALL rebuild the index from the `$MFT` before serving it as current when the volume has no journal, the journal was deleted or recreated with another id, it wrapped past the records not read yet, those records are overwritten while reading, or more than a quarter of the index nodes are left over from deletions and renames.

#### Scenario: Journal wrapped
Given the USN journal of C: wrapped past the last record MyClean read
When the index is refreshed
Then it is rebuilt from the `$MFT` and reports why

#### Scenario: Journal recreated
Given the journal of C: was deleted and created again with another id
When the index is refreshed
Then it is rebuilt from the `$MFT`

### Requirement: Folder Size History
The system SHALL store, per volume, snapshots of the size on disk of every folder of at least 32 MiB in the store table `index_folder_size`, and MUST keep each snapshot for 32 days.

#### Scenario: Newest snapshot at a time
Given snapshots of C: taken 8 and 2 days ago
When the snapshot at 7 days ago is asked for
Then the snapshot taken 8 days ago is returned

#### Scenario: Retention
Given a snapshot taken 33 days ago
When retention runs
Then that snapshot is deleted and newer ones are kept

### Requirement: Folder Growth Per Period
The system SHALL report, per volume, the folders that grew the most over the last 24 hours, 7 days and 30 days, comparing the current folder sizes with the newest snapshot at least that old, listing the folder where the growth happened rather than the folders above it (a folder is left out when one of its subfolders accounts for at least 90 % of its growth), and MUST NOT list a folder that shrank.

#### Scenario: What grew in 7 days
Given `C:\Users\bo\AppData\Local\Temp` grew by 6 GB in the last 7 days and no other folder grew
When the owner asks what grew on C: in 7 days
Then that folder is listed first with +6 GB and none of the folders above it is listed

#### Scenario: Shrinking folder
Given a folder lost 2 GB in the last 24 hours
When the 24-hour growth list is shown
Then the folder is not listed

#### Scenario: History too short
Given the oldest snapshot of C: is 3 days old
When the 7-day growth of C: is asked for
Then no growth report is returned

### Requirement: Whole-Volume Work in the Background
Reading a whole volume (the `$MFT`, or scanner v1 over a volume) SHALL run at below-normal CPU priority and background I/O priority, and the regular test suite MUST NOT read a whole real volume.

#### Scenario: Elevated measurement
Given the owner runs the X7 measurement elevated with `MYCLEAN_X7_VOLUME=C:`
When the `$MFT` of C: is read
Then the process runs at below-normal CPU and background I/O priority

#### Scenario: Regular test run
Given the test suite runs without the X7 variables
When it reaches the real-volume tests
Then they are skipped with the reason
