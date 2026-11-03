# Input Watchdog Specification

## ADDED Requirements

### Requirement: Hook-Free Observation
The input watchdog SHALL run inside the tray process in the owner's session and MUST observe keystrokes only through raw input (`RIDEV_INPUTSINK`) and clipboard changes only through a clipboard format listener, both registered on a message-only (`HWND_MESSAGE`) window; it MUST NOT install any Windows hook, MUST NOT register as a clipboard viewer, MUST NOT create a visible window and MUST NOT open the clipboard.

#### Scenario: No hook installed
Given the watchdog is running
When the tray process's calls are inspected
Then it never called `SetWindowsHookEx`, `SetClipboardViewer` or `OpenClipboard`

#### Scenario: Watchdog adds no input latency
Given the watchdog is running
When the owner types
Then keystrokes do not pass through any MyClean code before reaching applications

#### Scenario: Nothing visible
Given the watchdog is running
When the owner looks at the desktop and the taskbar
Then no MyClean window exists apart from the tray icon, and focus never moves

### Requirement: Only Copy and Paste Timing Recorded
The watchdog MUST record only the timing of Ctrl+C and Ctrl+V and the incident context, and MUST NOT store any other keystroke or any clipboard content. The raw input handler MUST keep only the Ctrl and Alt state and the C and V key state and MUST discard every other key before any buffer or log. Clipboard formats SHALL be read with `GetUpdatedClipboardFormats` as format IDs and names only; the watchdog MUST NOT read clipboard data or format sizes, because that needs `OpenClipboard`, forces delayed rendering and maps the content into MyClean.

#### Scenario: Typing text
Given the watchdog is running
When the owner types a password in a browser
Then no keystroke from that typing is stored

#### Scenario: Only the chord passes the input filter
Given the raw input handler receives Ctrl down, a letter other than C or V, then Ctrl+C
When the keys are filtered
Then only one copy event leaves the filter and the other letter is not kept anywhere

#### Scenario: Copied content
Given the owner copies text that contains a secret
When the copy is measured
Then the stored data contains clipboard format IDs and names but neither the copied text nor any format size

### Requirement: Copy Latency
The watchdog SHALL measure copy latency as the time from a Ctrl+C keydown seen through raw input to the next clipboard update notification, with both ends stamped with the monotonic `QueryPerformanceCounter` clock when the message is received; it MUST NOT use `GetMessageTime` for latency, which has a 15–16 ms resolution. The input thread SHALL run at above-normal priority. A Ctrl+C with no clipboard update within 5 s SHALL produce no measurement, and a new Ctrl+C SHALL restart the measurement.

Raw input may arrive after the low-level keyboard hooks have run (not verified, owner test T2), so a copy latency measured from raw input may not include hook delay. The watchdog SHALL therefore also record the key's input lag: the tick count at receipt minus the message time of the `WM_INPUT`, at 16 ms resolution, as evidence for the slow-keyboard-hook mechanism.

#### Scenario: Fast copy
Given the watchdog is running
When the owner presses Ctrl+C and the clipboard updates 40 ms later
Then a copy latency of about 40 ms is measured and no incident is recorded

#### Scenario: Ctrl+C that copies nothing
Given the watchdog is running
When the owner presses Ctrl+C in a terminal and the clipboard does not change within 5 s
Then no copy latency is measured and no incident is recorded

### Requirement: Paste Stall
The watchdog SHALL measure paste stall after a Ctrl+V keydown by probing the foreground window, and the clipboard owner (`GetClipboardOwner`) when it belongs to a different thread, with `SendMessageTimeout(WM_NULL)` with a 100 ms timeout every 100 ms, for up to 5 s. The paste stall is the time until the foreground window answers a probe; the owner's unanswered time is the time until the owner answers a probe and is recorded separately, because a pasting app that waits for a delayed render still answers `WM_NULL` while the owner does not. Probes run on a thread other than the input thread, so that they never delay the receipt time stamps.

#### Scenario: Pasting app stalls
Given the watchdog is running
When the owner presses Ctrl+V and the foreground window first answers a probe 800 ms later
Then a paste stall of about 800 ms is measured

#### Scenario: Foreground window never answers
Given the watchdog is running
When the owner presses Ctrl+V and the foreground window answers no probe for 5 s
Then probing stops at 5 s and the stall is recorded as at least 5 s

#### Scenario: Clipboard owner busy while the pasting app answers
Given the watchdog is running and the clipboard owner is another application
When the owner presses Ctrl+V, the foreground window answers at once and the clipboard owner first answers 700 ms later
Then a paste stall near 0 ms and an owner unanswered time of about 700 ms are recorded as an incident

### Requirement: Incident Recording
The watchdog SHALL record an incident whenever a copy latency, a paste stall, an owner unanswered time or the key's input lag exceeds 250 ms, capturing the key time, the measured times, the foreground application, the clipboard owner and, on paste, how long it did not answer, a process other than the foreground application that held the clipboard open during the stall, the viewer-chain head, the clipboard format IDs and names with synthesized formats marked (no sizes), the processes with hung top-level windows (`IsHungAppWindow`), which known suspect processes were running, and the machine's pressure state: total CPU utilisation during the stall and the memory load. Incidents SHALL be stored in `input_incident` in the service store for 2 years.

#### Scenario: Incident above threshold
Given the watchdog is running
When a paste stall of 600 ms is measured
Then an incident is stored with all listed context fields, empty fields marked as none

#### Scenario: Below threshold
Given the watchdog is running
When a copy latency of 250 ms or less is measured
Then no incident is stored

#### Scenario: Busy machine
Given the CPU was saturated during a 400 ms copy latency
When the incident is stored
Then it carries a CPU utilisation near 100 % and the memory load

### Requirement: Mechanism Classification
The system SHALL classify each incident into one of the mechanisms delayed rendering, clipboard held open, viewer chain, slow keyboard hook or heavy payload from the recorded fingerprints, checked in that order, SHALL list the evidence used, and SHALL classify it as unexplained when no fingerprint matches. Heavy payload SHALL key on the copied format set (bitmap, metafile, TIFF, PNG, JFIF, GIF, HTML or RTF formats that are not synthesized), never on byte sizes. Slow keyboard hook SHALL key on the key's input lag; this fingerprint is provisional until owner test T2 shows whether the message time of `WM_INPUT` is the input event time. `IsHungAppWindow` (which flips only after about 5 s) SHALL be evidence only, never a fingerprint for sub-5 s stalls.

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

#### Scenario: Slow keyboard hook
Given an incident whose Ctrl+C reached the watchdog 600 ms after its input time
When the incident is classified
Then its mechanism is slow keyboard hook and the running suspect processes are listed as evidence

#### Scenario: Heavy payload
Given a copy incident whose formats include PNG
When the incident is classified
Then its mechanism is heavy payload and the copying process is named

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
