# Input Watchdog Specification

## ADDED Requirements

### Requirement: Guided Elimination Offer
Once freeze incidents have a baseline rate, the system SHALL offer to pause one suspect process at a time, ordered by suspect rank, and SHALL NOT offer guided elimination before a baseline exists.

#### Scenario: Offer after baseline
Given copy/paste freeze incidents have a recorded baseline rate
And PowerToys Keyboard Manager is the top-ranked running suspect
When the owner opens the Freezes view
Then MyClean offers to pause PowerToys Keyboard Manager for a stated period

#### Scenario: No baseline yet
Given no baseline incident rate has been recorded
When the owner opens the Freezes view
Then no pause is offered and the view says a baseline is still being collected

### Requirement: Pause Requires Tray Approval
Each suspect pause SHALL be a plan approved by the owner in the tray before it runs; an MCP client MUST NOT approve or start a pause.

#### Scenario: Pause waits for approval
Given a pause of AnyDesk is proposed
When the owner has not approved it
Then AnyDesk keeps running

#### Scenario: MCP cannot start a pause
Given an AI client asks through `myclean mcp` to pause a suspect
When the request is handled
Then at most a plan in state `awaiting_approval` is created
And the suspect keeps running

### Requirement: One Suspect at a Time
The system MUST NOT run more than one suspect pause at the same time.

#### Scenario: Second pause refused
Given a pause of PowerToys Keyboard Manager is active
When the owner tries to start a pause of AnyDesk
Then the second pause is not started until the first one has been restored

### Requirement: Automatic Restore
The system SHALL restore a paused suspect automatically at the end of its pause period, and SHALL complete a pending restore after the tray or service restarts.

#### Scenario: Restore at end of period
Given PowerToys Keyboard Manager was paused for one day
When the day ends
Then PowerToys Keyboard Manager is running again and the restore is logged

#### Scenario: Restore survives a restart
Given a pause was active when the machine restarted
When MyClean starts again after the pause period ended
Then the suspect is restored and the restore is logged

#### Scenario: Owner ends a pause early
Given a pause is active
When the owner chooses to end it
Then the suspect is restored immediately

### Requirement: Incident Rate Comparison
After each pause the system SHALL compare the incident rate during the pause with the baseline rate and record both rates, the incident counts and the observed hours with the suspect.

#### Scenario: Rate drops during pause
Given the baseline rate is 4 incidents per day
And 0 incidents occurred during a one-day pause of PowerToys Keyboard Manager
When the comparison is made
Then the Freezes view shows 4 per day before and 0 during the pause for PowerToys Keyboard Manager

#### Scenario: No change
Given the incident rate during a pause of Discord equals the baseline
When the comparison is made
Then Discord is shown with no change in incident rate
