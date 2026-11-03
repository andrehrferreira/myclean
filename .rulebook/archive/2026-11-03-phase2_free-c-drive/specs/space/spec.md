# Space Specification

## ADDED Requirements

### Requirement: Safe Scanner Enumeration
The scanner SHALL enumerate any volume or folder in parallel per directory without descending into reparse points, SHALL report sizes as size on disk, and MUST NOT hydrate cloud placeholders.

#### Scenario: Junction is not traversed
Given a scanned folder contains a pnpm `node_modules` junction pointing to a store on another volume
When the scanner enumerates the folder
Then the junction is listed as a link entry
And no file under the junction target is enumerated or counted in the folder size

#### Scenario: Cloud placeholder is skipped
Given a OneDrive file carries FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
When the scanner enumerates its folder
Then the file is not opened or read
And no download is triggered
And its size is reported as its size on disk

#### Scenario: Sparse or compressed file size
Given a 10 GB file that occupies 2 GB on disk because of NTFS compression
When the scanner reports it
Then its reported size is 2 GB

### Requirement: Top-N Files and Folders
The system SHALL report the N largest files and the N largest folders of any volume or path, by size on disk, with N defaulting to 20.

#### Scenario: Top 20 on C:
Given the owner asks for the largest items on C:
When the space analysis runs
Then it returns the 20 largest files and the 20 largest folders on C:
And each entry has its full path and its size on disk

#### Scenario: Scoped to a path
Given the owner asks for the top 5 items under `F:\projects`
When the space analysis runs
Then every returned entry is located under `F:\projects`

### Requirement: Old Downloads
The system SHALL list files in the owner's Downloads folder whose creation and modification times are both older than 90 days, MUST NOT use last-access time to judge age, and SHALL show each file's origin from its Zone.Identifier stream when present.

#### Scenario: Old download listed with origin
Given `Downloads\setup.zip` was created and last modified 120 days ago
And its Zone.Identifier stream records a HostUrl
When old downloads are listed
Then `setup.zip` is listed with its size, age and HostUrl

#### Scenario: Recently modified file excluded
Given a Downloads file was created 200 days ago and modified 10 days ago
When old downloads are listed
Then the file is not listed

#### Scenario: Missing Zone.Identifier
Given an old download has no Zone.Identifier stream
When old downloads are listed
Then the file is listed with its origin shown as unknown

### Requirement: Installers Already Used
The system SHALL flag an `.msi` file as already used when its ProductCode is installed, and an `.exe` installer as already used when its version-info ProductName and CompanyName match an installed program's Uninstall entry.

#### Scenario: Installed MSI
Given `Downloads\tool.msi` has a ProductCode that MsiQueryProductState reports as installed
When used installers are listed
Then `tool.msi` is listed with its size and the matching installed product

#### Scenario: MSI not installed
Given an `.msi` whose ProductCode is not installed
When used installers are listed
Then the file is not listed

#### Scenario: EXE matched by version info
Given `AppSetup.exe` has ProductName "App" and CompanyName "Vendor"
And an Uninstall entry exists with DisplayName "App" and Publisher "Vendor"
When used installers are listed
Then `AppSetup.exe` is listed as already used

### Requirement: Exact Duplicates
The system SHALL detect exact duplicate files across all drives by grouping on size, then on a hash of the first 64 KB, then on a full BLAKE3 hash, and SHALL report only groups whose full BLAKE3 hashes are equal.

#### Scenario: Identical photos on two drives
Given `D:\photos\a.jpg` and `K:\archive\a-copy.jpg` have identical content
When duplicates are searched
Then both files appear in one duplicate group with their sizes and the reclaimable size

#### Scenario: Same prefix, different content
Given two files of equal size whose first 64 KB match but whose remaining content differs
When duplicates are searched
Then the files are not reported as duplicates

#### Scenario: Unique size short-circuits hashing
Given a file whose size matches no other scanned file
When duplicates are searched
Then the file is not read for hashing

### Requirement: Space Analysis Is Read-Only
Space analysis MUST NOT modify, move or delete any file; any removal of a listed item SHALL happen only through a cleaning plan approved by the owner.

#### Scenario: Prompt 3 executes nothing
Given the owner runs prompt 3 (top 20, old downloads, duplicate photos, used installers)
When all four lists are returned
Then the audit log shows no file operation
And every listed file still exists unchanged
