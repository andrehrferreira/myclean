# Drive Maintenance Specification

## ADDED Requirements

### Requirement: Read-Only Drive Maintenance Analysis
The system SHALL analyse every volume with a drive letter without changing anything: its medium (SSD or HDD, from the disk's seek penalty) and drive class, the disk's TRIM support and whether NTFS sends TRIM, the newest successful retrim and defragmentation from the storage optimizer's event log, and for an HDD a fragmentation sample and, when the volume bitmap is readable, its free-space layout. A source that cannot be read MUST be noted on the volume, never fail the analysis.

#### Scenario: SSD with its last retrim
Given D: is on an NVMe SSD that accepts TRIM
And the Application log holds a successful retrim of D: on 2026-10-29
When the drives are analysed
Then D: is reported as an SSD with TRIM supported and last retrim 2026-10-29
And D: is not sampled for fragmentation

#### Scenario: Optimizer log in another display language
Given the optimizer logged its operation names in Portuguese ("otimizar novamente", "desfragmentação")
When the log is interpreted
Then each run is classified as retrim or defragmentation from the operation code in the event's binary data

#### Scenario: Bitmap not readable without elevation
Given the analysis runs without administrator rights
When K: (an HDD) is analysed
Then its fragmentation sample is reported
And its free-space layout is missing with the reason "opening the volume K:\: Win32 error 5"

### Requirement: Bounded Fragmentation Sampling
The system SHALL estimate HDD fragmentation by reading the cluster runs (FSCTL_GET_RETRIEVAL_POINTERS, file opened for attributes only) of files of at least 64 MiB found in a breadth-first walk of the volume, within a budget of folders, files and time, at background CPU and I/O priority. Links MUST NOT be followed and cloud placeholders MUST NOT be opened. A file SHALL count as fragmented with more than 8 fragments and more than 8 per GiB; the sample reports fragments per file and the share of sampled bytes in fragmented files.

#### Scenario: Budget reached
Given the sample budget is 500 files
When K: holds more than 500 files of at least 64 MiB
Then 500 files are sampled and the sample is marked as stopped by its budget

#### Scenario: Large file in a few pieces
Given a 2 GiB file is stored in 16 fragments
When it is sampled
Then it does not count as fragmented

#### Scenario: Placeholder skipped
Given a OneDrive placeholder of 5 GiB lies on the volume
When the volume is sampled
Then the placeholder is not opened and not counted

### Requirement: Maintenance Recommendation
The system SHALL recommend a retrim for an SSD whose disk does not refuse TRIM when its last successful retrim is older than 30 days or none is in the log, a defragmentation for an HDD when at least 20 large files were sampled and more than 10 % of their bytes are in fragmented files, and nothing otherwise, each with its reasons. An SSD MUST NOT be recommended a defragmentation and an HDD MUST NOT be recommended a retrim. A drive whose health is not good SHALL be recommended nothing, with the maintenance it would need and its health in the reasons.

#### Scenario: Recently retrimmed SSD
Given W: was retrimmed 2 days ago
When the recommendation is made
Then W: is recommended nothing and the reason names the last retrim

#### Scenario: Retrim overdue
Given D: was last retrimmed 40 days ago
When the recommendation is made
Then D: is recommended a retrim because the last one is older than 30 days

#### Scenario: Fragmented HDD
Given 25 % of the bytes sampled on K: are in fragmented files
When the recommendation is made
Then K: is recommended a defragmentation

#### Scenario: Degraded HDD not recommended
Given the disk behind K: has a new pending sector and K: is fragmented
When the recommendation is made
Then K: is recommended nothing
And the reasons say a defragmentation would be due and name the pending sector

### Requirement: Retrim and Defragmentation Actions
The system SHALL provide RetrimVolume (`defrag <volume> /L`, tier auto) and DefragVolume (`defrag <volume> /D`, tier ask), run by the service with Windows' storage optimizer at its default low priority. Before running, each MUST read the drive again and refuse when the medium does not take that maintenance, the drive's health is not good, the computer runs on battery, or a disk of the volume was less than 90 % idle over the last 10 minutes or has too little idle-time history. Preview SHALL be the volume's analysis, verify SHALL analyse again (days since the last retrim, or the fragmented share), and undo SHALL fail as not reversible.

#### Scenario: Retrim on an idle healthy SSD
Given D: is on a healthy SSD whose disk was 98 % idle over the last 10 minutes
And the computer runs on mains power
When RetrimVolume for D: is applied
Then "defrag D: /L" runs
And verification compares the days since the last retrim before and after

#### Scenario: Busy disk deferred
Given the disk behind D: was 40 % idle over the last 10 minutes
When RetrimVolume for D: is applied
Then nothing runs and the refusal says the disk was 40.0 % idle

#### Scenario: SSD never defragmented
Given D: is on an SSD
When DefragVolume for D: is previewed or applied
Then it is refused because an SSD is never defragmented

#### Scenario: On battery
Given the computer runs on battery
When DefragVolume for K: is applied
Then nothing runs and the refusal names the battery

#### Scenario: No undo
Given a retrim of D: was applied
When its undo is requested
Then it fails as not reversible
