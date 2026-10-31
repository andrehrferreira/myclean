# NTFS Index Specification

## ADDED Requirements

### Requirement: Index Built From the MFT
The service SHALL build an index of every NTFS volume by reading its `$MFT`, holding each folder's total size on disk, and MUST NOT open, read or hydrate the indexed files' contents.

#### Scenario: Initial build
Given C: is an NTFS volume with no index yet
When the service builds the index
Then every folder on C: has a recorded size on disk
And no file content is read and no cloud placeholder is hydrated

#### Scenario: Non-NTFS volume
Given a volume formatted with exFAT
When indexing runs
Then the volume is not indexed and is marked as using the regular scanner

### Requirement: Index Kept Current by the USN Journal
The service SHALL keep each index current by reading the volume's USN change journal (FSCTL_READ_USN_JOURNAL), and SHALL rebuild the index from the `$MFT` when the journal no longer covers the gap since the last read.

#### Scenario: Folder size updates
Given the index of F: is current
When a build writes 3 GB into `F:\game\target`
Then the indexed size of `F:\game\target` and of each parent folder grows by 3 GB without a rescan

#### Scenario: Journal gap triggers rebuild
Given the USN journal of C: was deleted or wrapped past the last record MyClean read while the service was stopped
When the service starts
Then the index of C: is rebuilt from the `$MFT` before it is served as current

### Requirement: Consistency With a Full Rescan
Folder sizes served by the index SHALL match a full rescan of the same volume taken at the same moment.

#### Scenario: C: and F: match a rescan
Given the index of C: and F: has run for a week of normal use
When a full rescan of each volume is compared with the index
Then the size of every folder in the index equals the rescanned size

### Requirement: Folder Growth Per Period
The system SHALL report, per volume, the folders that grew the most over the last 24 hours, 7 days and 30 days.

#### Scenario: What grew in 7 days
Given `C:\Users\<owner>\AppData\Local\Temp` grew by 6 GB in the last 7 days and no other folder grew more
When the owner asks what grew on C: in 7 days
Then that folder is listed first with +6 GB

#### Scenario: Shrinking folder
Given a folder lost 2 GB in the last 24 hours
When the 24-hour growth list is shown
Then the folder is not listed among the folders that grew

### Requirement: Instant Search
The system SHALL answer name searches and folder-size queries on an indexed volume from the index, without enumerating the volume.

#### Scenario: Search by name
Given C: is indexed
When the owner searches for `*.dmp`
Then matching files with their paths and sizes are returned from the index without a directory walk

### Requirement: Photo Near-Duplicates
The system SHALL find near-duplicate photos by decoding each image with WIC, computing a 64-bit dHash, and grouping images whose hashes differ in at most 6 bits, and SHALL only report the groups, never act on them.

#### Scenario: Resized copy found
Given `D:\photos\img.jpg` and a re-encoded half-size copy `K:\old\img_small.jpg`
When near-duplicates are searched
Then both images appear in one group

#### Scenario: Different photos not grouped
Given two photos whose dHash values differ in 12 bits
When near-duplicates are searched
Then they are not grouped

#### Scenario: Undecodable file
Given a file with a `.jpg` name that WIC cannot decode
When near-duplicates are searched
Then the file is skipped and listed as not analysed

#### Scenario: Report only
Given a near-duplicate group was found
When the search finishes
Then no file in the group has been moved or deleted
