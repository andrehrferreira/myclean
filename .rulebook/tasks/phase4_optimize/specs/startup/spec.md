# Startup Specification

## ADDED Requirements

### Requirement: Startup Inventory
The system SHALL list every automatic start source: Run and RunOnce keys in HKCU and HKLM including WOW6432Node, the user and common Startup folders, each item's StartupApproved state, scheduled tasks with logon or boot triggers read through ITaskService, and auto-start services not published by Microsoft.

#### Scenario: All sources listed
Given an HKLM WOW6432Node Run value, a shortcut in the common Startup folder, a task with a logon trigger and a third-party auto-start service exist
When the startup inventory runs
Then all four appear with their source, command and current enabled state

#### Scenario: Disabled in Task Manager
Given a Run entry whose StartupApproved value marks it disabled
When the startup inventory runs
Then the entry is listed as disabled

#### Scenario: Microsoft services excluded
Given an auto-start service signed by Microsoft Windows
When the startup inventory runs
Then the service is not listed as a startup item

### Requirement: Measured Startup Impact
The system SHALL show for each startup item its CPU and disk use during the first 2 minutes after logon from MyClean's own ETW history, and SHALL use Diagnostics-Performance/Operational events 100 and 101 where available until enough boots are recorded, labelling which source was used.

#### Scenario: Impact from own history
Given MyClean has recorded several logons with process history
When the inventory is shown
Then each item shows its CPU seconds and disk bytes in the first 2 minutes after logon, labelled as measured by MyClean

#### Scenario: Fallback before history exists
Given MyClean has recorded no logon yet
When the inventory is shown
Then items show impact from Diagnostics-Performance events where available, labelled with that source
And items with no data show impact as unknown

### Requirement: Disable and Enable Through Native Mechanisms
The system SHALL disable a Run-key or Startup-folder item by writing its StartupApproved state, a task by setting its Enabled flag to false, and a service by setting its start type to Manual, and SHALL enable each through the same mechanism; it MUST NOT delete the Run value, the shortcut, the task or the service.

#### Scenario: Run item disabled like Task Manager
Given an approved plan disables the HKCU Run entry "Discord"
When the plan executes
Then the StartupApproved value for "Discord" marks it disabled
And the Run value still exists
And Task Manager shows "Discord" as disabled

#### Scenario: Task disabled
Given an approved plan disables a logon task
When the plan executes
Then the task still exists with Enabled = false

#### Scenario: Service set to Manual
Given an approved plan disables a third-party auto-start service
When the plan executes
Then the service start type is Manual and the service is not deleted

### Requirement: Exact Startup Undo
The system SHALL record each item's previous state and undo SHALL restore exactly that state.

#### Scenario: Delayed-start service restored
Given a service had start type Automatic (Delayed Start) before being set to Manual
When the owner undoes the action
Then the service start type is Automatic (Delayed Start) again

#### Scenario: Run item re-enabled
Given a Run item was disabled through StartupApproved
When the owner undoes the action
Then StartupApproved marks it enabled and it starts at the next logon

### Requirement: Only Approved Items Change
The system SHALL change a startup item only as part of a plan approved in the tray; `propose_disable_startup` over MCP SHALL create a plan in state `awaiting_approval` and MUST NOT change any item by itself.

#### Scenario: Prompt 2 disables only the selection
Given the inventory lists 25 items and the owner approves disabling 3 of them
When the plan executes
Then exactly those 3 items are disabled
And the other 22 items keep their previous state

#### Scenario: MCP proposal waits
Given an AI client calls `propose_disable_startup` with two item ids
When the call returns
Then both items are still enabled
And a plan for them waits in the approval inbox

### Requirement: Boot and Logon Time Tracking
The system SHALL record the duration of each boot and logon and SHALL attribute delay to the startup items and services active during that window, using the same measured impact data.

#### Scenario: Slow logon attributed
Given the latest logon took 40 s longer than the 30-day median
And one startup item used most of the disk bytes in the first 2 minutes
When the boot/logon view is opened
Then the logon duration and its difference from the median are shown
And that startup item is listed first among the contributors

#### Scenario: Effect of disabling measured
Given a startup item was disabled through an approved plan
When the next logons are recorded
Then the logon duration before and after the change is shown for that action
