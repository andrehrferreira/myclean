# Telemetry Specification

## ADDED Requirements

### Requirement: CPU Sampling
The system SHALL sample the CPU every 1 s: total and per logical processor utilization, % of maximum frequency, processor queue length, and DPC and interrupt time.

#### Scenario: All logical processors
Given a CPU with 32 logical processors
When the CPU collector runs for 10 s
Then each of the 32 logical processors and the total have 10 samples of utilization and % of maximum frequency

#### Scenario: Throttling visible
Given a logical processor runs below its maximum frequency
When it is sampled
Then its % of maximum frequency sample is below 100

### Requirement: Memory Sampling
The system SHALL sample memory every 1 s: available memory, commit charge and commit limit, standby and modified lists, compressed memory, hard faults/s (`Pages Input/sec`) and pool usage.

#### Scenario: Commit sampled with its limit
Given the memory collector is running
When one sample is taken
Then it contains commit charge and commit limit from the same instant

### Requirement: Physical Disk Sampling
The system SHALL discover every physical disk at runtime, including disks on USB, and SHALL sample each every 1 s: read and write bytes/s, IOPS, average latency per read and per write, queue length and % idle time.

#### Scenario: Eight disks
Given the reference machine with 5 internal NVMe disks, 1 USB SSD and 2 SATA HDDs
When the disk collector starts
Then 8 physical disks are discovered and each gets its own 1 s series, with no disk number or model hard-coded

### Requirement: Volume Sampling
The system SHALL sample every volume every 60 s (size and free space), map each volume to its physical disk(s), and compute a free-space trend and a days-until-full projection.

#### Scenario: Filling volume
Given volume C: lost free space steadily over the last 7 days
When the volume collector computes the trend
Then C: has a days-until-full projection and is mapped to its physical disk

#### Scenario: Volume not filling
Given a volume whose free space has not decreased over the trend window
When the projection is computed
Then the volume is reported as not filling instead of with a date

### Requirement: Process Table and Trees
The system SHALL read the full process table every 1 s with one `NtQuerySystemInformation(SystemProcessInformation)` call, recording CPU, private bytes, working set, I/O bytes, handles and threads per process, and SHALL group processes into trees whose parent links are safe against PID reuse.

#### Scenario: Browser grouped into one tree
Given a browser process with 30 child processes
When the process table is sampled
Then the browser and its 30 children form one tree whose totals sum its members

#### Scenario: Reused parent PID
Given a process whose parent has exited and whose parent PID now belongs to a process created after it
When trees are built
Then the process is not attached to the newer process and becomes the root of its own tree

### Requirement: Process Events
The service SHALL receive process start and stop events in real time from the ETW `Microsoft-Windows-Kernel-Process` provider, each with image, command line, parent and signer, and SHALL persist every start in `proc_start`.

#### Scenario: Short-lived process captured
Given the ETW session is running
When a process starts and exits within 200 ms
Then both its start and stop events are delivered and its start is stored in `proc_start`

### Requirement: GPU and Network Sampling
The system SHALL sample GPU per-engine utilization and dedicated and shared memory, per adapter and per process, every 1 s, and SHALL record network bytes sent and received per process from the ETW `Microsoft-Windows-Kernel-Network` provider.

#### Scenario: Per-process GPU
Given a game uses the 3D engine
When the GPU collector samples
Then the game's process has its own 3D engine utilization and dedicated memory values

#### Scenario: Per-process network
Given a background process uploads 10 MB
When the network data for that minute is queried
Then the process shows about 10 MB sent

### Requirement: Non-Blocking Sample Bus
Each collector SHALL publish into its own lock-free single-producer ring, and a slow or stalled subscriber MUST NOT delay any collector.

#### Scenario: Stalled subscriber
Given a dashboard subscriber stops reading for 30 s
When collectors keep sampling at 1 s
Then no collector misses a sample and the stalled subscriber resumes from the newest data

### Requirement: History Retention and Downsampling
The system SHALL keep 1 s raw samples for 1 h in memory, 1 min aggregates for 30 days and 1 h aggregates for 2 years, and SHALL write history to the database in batches every 10 s.

#### Scenario: Aging out
Given a metric has been collected for 3 years
When its history is inspected
Then raw samples cover only the last hour, 1 min aggregates the last 30 days and 1 h aggregates the last 2 years

### Requirement: History Query
The service SHALL answer history queries by metric, entity and time range, choosing the finest resolution that the retention covers for that range.

#### Scenario: Last 7 days
Given 30 days of history for disk 2 latency
When a client queries the last 7 days
Then the answer uses 1 min aggregates

#### Scenario: Last 6 months
Given 1 year of history for C: free space
When a client queries the last 6 months
Then the answer uses 1 h aggregates
