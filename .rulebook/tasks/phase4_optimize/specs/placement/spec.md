# Placement Specification

## ADDED Requirements

### Requirement: Workload Placement Advisor
The system SHALL recommend which drive each workload belongs on from each drive's class (NVMe, QLC, USB, HDD), health and free space, explaining the reason for every recommendation.

#### Scenario: Build output off QLC
Given active build-output folders live on W:, a QLC DRAM-less NVMe drive
And F: is a healthy TLC NVMe drive with enough free space
When the placement advisor runs
Then it recommends moving build output from W: to a non-QLC NVMe drive such as F:
And the reason mentions QLC write performance

#### Scenario: Archives to HDD
Given a 200 GB archive folder untouched for months lives on D: (NVMe)
And L: is a healthy HDD with 5.9 TB free
When the placement advisor runs
Then it recommends L: for the archive folder

#### Scenario: Not enough free space
Given the best-class drive for a workload has less free space than the workload's size
When the placement advisor runs
Then that drive is not recommended for the workload

### Requirement: Placement Advice Excludes Unhealthy Drives
The placement advisor MUST NOT recommend a drive whose health is degraded or unknown as a destination.

#### Scenario: Degraded HDD excluded
Given the disk behind K: has a new reallocated sector
When the placement advisor runs
Then no recommendation names K: as a destination
And the advice notes that K: is excluded because of its health

### Requirement: Placement Advice Is Advice Only
The placement advisor SHALL only recommend; it MUST NOT move any data itself, and any change it leads to SHALL go through a separate owner-approved action.

#### Scenario: No data moved
Given the advisor recommended moving a folder from W: to F:
When the advisor finishes
Then the folder is still on W: and the audit log shows no file operation from the advisor

### Requirement: Cold-Data Report
The system SHALL list large folders on NVMe volumes that have not been modified for a configurable number of months as candidates to move to an HDD, naming only healthy HDDs as targets, and MUST NOT act on them.

#### Scenario: Cold folder listed
Given `G:\renders\2024` is 300 GB and nothing in it changed for 9 months
And the cold-data threshold is 6 months
When the cold-data report runs
Then the folder is listed with its size, last-change date and L: as a candidate target

#### Scenario: Recently changed folder
Given a large folder with a file changed last week
When the cold-data report runs
Then the folder is not listed

#### Scenario: Degraded target excluded
Given both HDDs have degraded health
When the cold-data report runs
Then folders are listed without a target and the report says no healthy HDD is available

### Requirement: Dev Drive Advisor
The system SHALL explain what a Dev Drive is (a ReFS volume with Microsoft Defender performance mode), estimate which active repositories and package caches would move onto it and their size, and guide the owner through creating one and moving them, and MUST NOT create, format, shrink or resize any volume or partition itself.

#### Scenario: No Dev Drive present
Given every volume is NTFS
When the owner opens the Dev Drive advisor
Then it states that no Dev Drive exists
And it lists the active repositories and package caches that would move onto one with their total size
And it shows step-by-step instructions for creating one

#### Scenario: Never creates a volume
Given the owner reads the Dev Drive guide to the end
When the advisor finishes
Then the partition and volume layout of every disk is unchanged
And no format, shrink or create-volume command was run by MyClean
