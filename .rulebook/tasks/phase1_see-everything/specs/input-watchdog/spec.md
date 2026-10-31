# Input Watchdog Specification

## ADDED Requirements

### Requirement: Hook-Free Observation
The input watchdog SHALL run inside the tray process in the owner's session and MUST observe keystrokes only through raw input (`RIDEV_INPUTSINK`) and clipboard changes only through a clipboard format listener; it MUST NOT install any Windows hook and MUST NOT register as a clipboard viewer.

#### Scenario: No hook installed
Given the watchdog is running
When the tray process's calls are inspected
Then it never called `SetWindowsHookEx` and never called `SetClipboardViewer`

#### Scenario: Watchdog adds no input latency
Given the watchdog is running
When the owner types
Then keystrokes do not pass through any MyClean code before reaching applications

### Requirement: Only Copy and Paste Timing Recorded
The watchdog MUST record only the timing of Ctrl+C and Ctrl+V and the incident context, and MUST NOT store any other keystroke or any clipboard content.

#### Scenario: Typing text
Given the watchdog is running
When the owner types a password in a browser
Then no keystroke from that typing is stored

#### Scenario: Copied content
Given the owner copies text that contains a secret
When the copy is measured
Then the stored data contains clipboard format names and sizes but not the copied text

### Requirement: Copy Latency
The watchdog SHALL measure copy latency as the time from a Ctrl+C keydown seen through raw input to the next clipboard update notification.

#### Scenario: Fast copy
Given the watchdog is running
When the owner presses Ctrl+C and the clipboard updates 40 ms later
Then a copy latency of about 40 ms is measured and no incident is recorded

### Requirement: Paste Stall
The watchdog SHALL measure paste stall after a Ctrl+V keydown by probing the foreground window with `SendMessageTimeout(WM_NULL)` with a 100 ms timeout every 100 ms, for up to 5 s, and taking the time until a probe is answered.

#### Scenario: Pasting app stalls
Given the watchdog is running
When the owner presses Ctrl+V and the foreground window first answers a probe 800 ms later
Then a paste stall of about 800 ms is measured

#### Scenario: Foreground window never answers
Given the watchdog is running
When the owner presses Ctrl+V and the foreground window answers no probe for 5 s
Then probing stops at 5 s and the stall is recorded as at least 5 s

### Requirement: Incident Recording
The watchdog SHALL record an incident whenever a copy latency or paste stall exceeds 250 ms, capturing timestamps, the foreground application, the clipboard owner and whether it responds, the window holding the clipboard open, the viewer-chain head, the clipboard formats and their sizes, the hung top-level windows, and which known suspect processes were running; incidents SHALL be stored in `input_incident` in the service store for 2 years.

#### Scenario: Incident above threshold
Given the watchdog is running
When a paste stall of 600 ms is measured
Then an incident is stored with all listed context fields, empty fields marked as none

#### Scenario: Below threshold
Given the watchdog is running
When a copy latency of 250 ms or less is measured
Then no incident is stored

### Requirement: Mechanism Classification
The system SHALL classify each incident into one of the mechanisms delayed rendering, clipboard held open, viewer chain, slow keyboard hook or heavy payload from the recorded fingerprints, SHALL list the evidence used, and SHALL classify it as unexplained when no fingerprint matches.

#### Scenario: Delayed rendering
Given an incident on paste where the clipboard owner did not answer `SendMessageTimeout(WM_NULL)`
When the incident is classified
Then its mechanism is delayed rendering and the clipboard owner process is named as the cause

#### Scenario: Clipboard held open
Given an incident where a window of another process held the clipboard open during the stall
When the incident is classified
Then its mechanism is clipboard held open and that process is named

#### Scenario: Viewer chain
Given an incident on copy with a non-empty clipboard viewer chain
When the incident is classified
Then its mechanism is viewer chain and the viewer-chain head process is named

#### Scenario: No fingerprint
Given an incident that matches none of the fingerprints
When the incident is classified
Then it is shown as unexplained with its raw context, not forced into a mechanism

### Requirement: Freeze View
The tray SHALL provide a Freezes view that lists incidents grouped by mechanism and by the process involved, with counts, latest occurrence and each incident's evidence.

#### Scenario: Grouped incidents
Given 5 incidents attributed to clipboard held open by one process and 2 unexplained incidents
When the owner opens the Freezes view
Then one group shows the 5 incidents with that process, another shows the 2 unexplained incidents, and each incident opens to its evidence

#### Scenario: No incidents yet
Given the watchdog has recorded no incident
When the owner opens the Freezes view
Then it states that no freeze over 250 ms has been recorded since the watchdog started
