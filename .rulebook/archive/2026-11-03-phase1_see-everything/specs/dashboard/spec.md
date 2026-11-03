# Dashboard Specification

## ADDED Requirements

### Requirement: Tray Icon State and Tooltip
The tray icon SHALL show a state color derived from the most severe active alert, and a tooltip with current CPU, RAM, commit, C: free space and the worst disk-health status.

#### Scenario: Tooltip contents
Given the service is running
When the owner hovers over the tray icon
Then the tooltip shows CPU %, RAM in use, commit charge, C: free space and the drive with the worst health status

#### Scenario: Alert changes the color
Given no alert is active
When a disk-health alert is raised
Then the tray icon color changes to the alert's severity

### Requirement: Dashboard Responsiveness
The dashboard SHALL open in less than 1 s and SHALL update its live charts at 1 Hz without dropping samples.

#### Scenario: Opening
Given the tray is running
When the owner opens the dashboard
Then it is displayed with current data in less than 1 s

#### Scenario: No dropped samples
Given the dashboard has been open for 10 min
When its CPU chart is compared with the service's raw samples
Then every 1 s sample appears in the chart

### Requirement: CPU, Memory and GPU Charts
The dashboard SHALL show live charts of CPU per logical processor with % of maximum frequency, memory (available, commit charge versus limit, standby, hard faults/s) and GPU engine utilization and memory.

#### Scenario: Per-core throttling
Given one logical processor runs at 70 % of maximum frequency
When the CPU chart is shown
Then that processor's frequency reads about 70 %

### Requirement: Per-Disk Charts
The dashboard SHALL show a live chart for each physical disk with throughput, IOPS, read and write latency, queue length and % idle.

#### Scenario: All disks charted
Given the reference machine with 8 physical disks
When the owner opens the dashboard
Then 8 disk charts are shown, each labeled with its disk number, model and volumes

### Requirement: Drive Health Cards
The dashboard SHALL show a health card for each physical disk with its health status, temperature, wear, error counters and endurance projection, or an explicit "not readable over USB" with the reliability counters available.

#### Scenario: USB drive without SMART
Given the USB SSD's bridge blocks SMART
When its health card is shown
Then the card says "not readable over USB" and shows the reliability-counter values instead of empty fields

#### Scenario: Degrading drive
Given a drive reported a new reallocated sector today
When its health card is shown
Then the card highlights the changed counter with its previous and current values

### Requirement: Volume Bars
The dashboard SHALL show a bar per volume with size, used and free space and its days-until-full projection, or "not filling" when there is no projection.

#### Scenario: C: filling up
Given C: has 84 GB free of 952 GB and a projection of 40 days
When the volume bars are shown
Then C: shows 84 GB free and 40 days until full

### Requirement: Top Process Trees
The dashboard SHALL show the top process trees by CPU and by memory, expandable to their members, with each tree's trust flag.

#### Scenario: Expanding a tree
Given a browser tree is in the top list
When the owner expands it
Then its child processes are listed with their own CPU and memory

#### Scenario: Unrecognized process visible
Given an unrecognized process is in the top list
When the list is shown
Then the process carries the unrecognized flag and its reasons

### Requirement: History Ranges
The dashboard charts SHALL let the owner switch from the live view to past ranges served by the history query API.

#### Scenario: Last 30 days
Given 30 days of history exist
When the owner selects the last 30 days on a disk chart
Then the chart shows that range from stored aggregates

### Requirement: Service Unavailable
When the service is unreachable the dashboard MUST state that live data is unavailable and MUST NOT present stale values as live.

#### Scenario: Service stops while open
Given the dashboard is open
When the service stops
Then the charts stop advancing, a banner names the unavailable features, and the last values are labeled with their time
