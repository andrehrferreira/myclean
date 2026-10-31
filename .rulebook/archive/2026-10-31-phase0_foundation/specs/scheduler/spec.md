# Scheduler Specification

## ADDED Requirements

### Requirement: Cron Expression Parsing
The scheduler MUST parse five-field cron expressions (minute, hour, day of month, month, day of week) with lists, ranges and steps, and MUST reject an invalid expression with an error naming the field, without registering the job.

#### Scenario: Monthly expression
Given the expression `0 9 1 * *`
When the next run after 2026-10-31 12:00 local time is computed
Then the result is 2026-11-01 09:00 local time

#### Scenario: Steps and ranges
Given the expression `*/15 8-18 * * 1-5`
When the next run after Friday 18:50 is computed
Then the result is the following Monday 08:00

#### Scenario: Invalid expression rejected
Given the expression `0 25 * * *`
When a job is registered with it
Then registration fails with an error naming the hour field and no job is stored

### Requirement: Persisted Schedule State
The scheduler MUST persist each job's expression, last run time, last result and next run time in the `schedule` table, and MUST restore them on start.

#### Scenario: State survives restart
Given a job whose last run was 2026-10-01 09:00 and next run is 2026-11-01 09:00
When the service restarts on 2026-10-15
Then the job's last and next run times are read back unchanged

### Requirement: Catch-Up After Downtime
When the scheduler starts and finds that a job's next run time has passed while MyClean was not running, it MUST run that job once as a catch-up, then compute the next run from the current time; several missed occurrences of the same job SHALL be coalesced into a single run.

#### Scenario: Monthly job missed while the machine was off
Given a monthly job `0 9 1 * *` whose last run was 2026-10-01 09:00
And the machine was off from 2026-10-31 until 2026-11-03 08:00
When the service starts on 2026-11-03 08:00
Then the job runs once as a catch-up, marked as such in its run record
And its next run is 2026-12-01 09:00

#### Scenario: Several missed occurrences coalesced
Given an hourly job and a downtime of 5 hours
When the service starts
Then the job runs exactly once as a catch-up

#### Scenario: Nothing missed
Given a job whose next run is in the future
When the service starts
Then the job does not run until its next run time

### Requirement: Clock Changes
The scheduler MUST compute run times in local time and SHALL run each occurrence at most once across daylight-saving changes and manual clock changes.

#### Scenario: Repeated hour at end of daylight saving
Given a job `30 2 * * *` on a night when 02:30 occurs twice
When the night passes
Then the job runs once

### Requirement: No Overlapping Runs
The scheduler MUST NOT start a job while a previous run of the same job is still in progress; the due occurrence SHALL be recorded as skipped with that reason.

#### Scenario: Long run blocks the next occurrence
Given a job every 15 min whose current run has lasted 20 min
When the next occurrence becomes due
Then no second run starts and the occurrence is recorded as skipped because the previous run is still active

### Requirement: No OS Scheduler
All timing MUST live in MyClean's own scheduler; MyClean SHALL NOT create, modify or depend on Task Scheduler tasks, cron, `schtasks` or any other operating-system schedule.

#### Scenario: Registering a job creates no OS task
Given the Task Scheduler task list before the service starts
When the service starts and registers its jobs
Then the Task Scheduler task list is unchanged

### Requirement: Testable Time
The scheduler MUST take the current time from an injectable clock so that next-run, catch-up and clock-change behavior can be tested without waiting in real time.

#### Scenario: Catch-up tested with a fake clock
Given a fake clock set to 2026-11-03 08:00 and a stored job last run 2026-10-01 09:00
When the scheduler starts in a unit test
Then the catch-up run is triggered without any real-time wait
