# Optimizer Specification

## ADDED Requirements

### Requirement: Drive Maintenance Only When Idle and Healthy
The system SHALL re-TRIM SSD volumes and defragment HDD volumes only while the drive is idle and its health is known and not degraded, MUST NOT defragment an SSD, and SHALL record each run's result.

#### Scenario: Re-TRIM on an idle SSD
Given D: is on a healthy NVMe SSD and its disk is idle
When drive maintenance is due
Then D: is re-trimmed
And the result, duration and time are recorded

#### Scenario: Busy drive deferred
Given a build is writing to F: when maintenance is due
When the maintenance check runs
Then F: is not re-trimmed now and the run is deferred until the drive is idle

#### Scenario: Degraded HDD skipped
Given the disk behind K: has a new pending sector
When HDD defragmentation is due
Then K: is not defragmented
And the skip and its reason are recorded

#### Scenario: SSD never defragmented
Given W: is an SSD
When drive maintenance runs
Then W: is only re-trimmed and never defragmented

### Requirement: Search-Indexer Scope
The system SHALL detect build-output and cache folders that are inside the Windows Search indexed scope and SHALL offer their exclusion as an action with undo that applies only after owner approval.

#### Scenario: Indexed node_modules detected
Given `F:\app\node_modules` is inside an indexed scope
When the indexer-scope check runs
Then the folder is listed with an "exclude from index" proposal

#### Scenario: Exclusion undone
Given an approved exclusion of `F:\app\target` was applied
When the owner undoes it
Then the indexer scope is the same as before the exclusion

### Requirement: Memory-Leak Detection
The system SHALL alert, with the trend chart, when a process tree's private bytes grow steadily for hours, and MUST NOT act on the process.

#### Scenario: Sustained growth alerts
Given a resident process tree's private bytes rose steadily over 6 hours without returning to its earlier level
When the leak detector evaluates the history
Then an alert names the tree and shows its private-bytes trend

#### Scenario: Load spike is not a leak
Given a tree's private bytes rose during a build and fell back within the hour
When the leak detector evaluates the history
Then no leak alert is raised

#### Scenario: Detection only
Given a leak alert was raised for a tree
When the alert is shown
Then the tree has not been suspended, capped or killed by the detector

### Requirement: Background-App Cost Report
The system SHALL report every resident process with its CPU, RAM, disk and network cost over the last 30 days.

#### Scenario: 30-day costs
Given Armoury Crate processes ran resident for the last 30 days
When the background-app report is opened
Then Armoury Crate is listed with its 30-day CPU time, average and peak RAM, disk bytes and network bytes

### Requirement: Before-and-After Verification
Every applied optimization SHALL have its targeted metric measured before and after it is applied, and the verification result SHALL be stored with the action in the audit log.

#### Scenario: Verification recorded
Given an approved action disables a startup item to shorten logon
When the action has been applied and the next logon is measured
Then the audit log holds the logon duration before and after and a verification result

#### Scenario: Pending verification visible
Given an action was applied but its metric has not been measured again yet
When the audit view is opened
Then the action shows its verification as pending, not as passed

### Requirement: Auto-Undo When Worse
When a verification shows the targeted metric got worse, the system SHALL undo the action automatically if it is reversible and notify the owner, and SHALL alert the owner without undoing if it is not reversible.

#### Scenario: Reversible and worse
Given an indexer exclusion was applied and the measured metric got worse afterwards
When verification completes
Then the exclusion is undone automatically
And the plan ends in state `rolled_back` with a notification to the owner

#### Scenario: Not reversible and worse
Given a cache purge was applied and the targeted metric got worse
When verification completes
Then the owner is alerted
And no undo is attempted

#### Scenario: Better or unchanged
Given an applied action whose metric improved
When verification completes
Then the plan ends in state `verified` and nothing is undone

### Requirement: VHDX Size Report and Compaction
The system SHALL report the size of every WSL and Docker `.vhdx`, and SHALL compact one only as an `ask`-tier plan approved by the owner that warns that running distros and containers will stop, runs `wsl --shutdown` and then compacts the disk.

#### Scenario: Report
Given a WSL distro's `ext4.vhdx` is 120 GB
When the VHDX report is opened
Then the file is listed with its path and 120 GB

#### Scenario: Compaction needs approval
Given a WSL distro is running
When a compaction plan is created
Then it waits in the approval inbox with a warning that running distros and containers will stop
And `wsl --shutdown` is not run until the owner approves

#### Scenario: Approved compaction
Given the owner approved compacting a VHDX
When the plan executes
Then `wsl --shutdown` runs first, the disk is compacted, and the size before and after is recorded
