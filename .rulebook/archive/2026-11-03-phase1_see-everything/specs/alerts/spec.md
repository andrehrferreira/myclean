# Alerts Specification

## ADDED Requirements

### Requirement: Alert Rules
The service SHALL evaluate alert rules of two kinds against telemetry and health data: threshold rules (a value beyond a limit) and change rules (a counter or state that changed since the previous reading), with limits loaded from data files rather than compiled in.

#### Scenario: Threshold rule
Given a rule that fires when C: free space falls below 10 %
When C: free space drops to 9 %
Then an alert is raised for volume C: with the measured value and the limit

#### Scenario: Change rule
Given a change rule on a drive's media error count
When the count rises from 0 to 1
Then an alert is raised even though no threshold is crossed

### Requirement: Volume Fill-Up Alerts
The service SHALL raise an alert when a volume's days-until-full projection falls below the configured limit.

#### Scenario: Volume about to fill
Given the fill-up limit is configured as 14 days
When C: is projected to be full in 9 days
Then an alert names C:, its free space and the 9-day projection

#### Scenario: Volume not filling
Given the fill-up limit is configured as 14 days
When a volume is reported as not filling
Then no fill-up alert is raised for it

### Requirement: Alert Record
Every raised alert SHALL be stored in the `alert` table for 2 years with its rule, entity, severity, values, first and last occurrence time and occurrence count, and SHALL be listable through the `alerts.list` IPC method.

#### Scenario: Listing alerts
Given 3 alerts were raised today
When a client calls `alerts.list`
Then all 3 are returned with rule, entity, severity, values and times

### Requirement: Delivery by Toast and Tray Badge
The tray SHALL show each new alert as a toast and SHALL show a badge on the tray icon while alerts are active; alerts raised while the tray is not running SHALL be kept and reflected in the badge when the tray connects.

#### Scenario: New alert with tray running
Given the tray is running and no alert is active
When the service raises a disk-health alert
Then a toast describes the alert and the tray icon shows a badge

#### Scenario: Tray not running
Given the tray is not running
When the service raises an alert
Then the alert is stored, and when the tray starts its icon shows the badge

### Requirement: Rate Limiting
The system MUST NOT show more than one toast for the same rule and entity within the rate-limit window; repeated firings inside the window SHALL only update the existing alert's last occurrence and count.

#### Scenario: Flapping threshold
Given the rate-limit window is 5 minutes
When the same rule fires for the same volume 12 times within 5 minutes
Then exactly one toast is shown and the stored alert has an occurrence count of 12

#### Scenario: Different entities are independent
Given the rate-limit window is 5 minutes
When the same temperature rule fires for disk 3 and for disk 5 within 1 minute
Then one toast is shown for each disk

### Requirement: Per-Alert Snooze
The owner SHALL be able to snooze an individual alert for a chosen duration; while snoozed, the alert MUST NOT produce toasts or count in the badge, but its occurrences SHALL still be recorded.

#### Scenario: Snoozed alert fires again
Given the owner snoozed the C: fill-up alert for 24 h
When the rule fires again 2 h later
Then no toast appears, the badge does not count it, and the occurrence is recorded

#### Scenario: Snooze expires
Given the owner snoozed an alert for 1 h
When the rule fires again after 1 h has passed
Then a toast is shown again

#### Scenario: Snooze is per alert
Given the owner snoozed the fill-up alert for C:
When a disk-health alert is raised for disk 2
Then its toast is shown normally
