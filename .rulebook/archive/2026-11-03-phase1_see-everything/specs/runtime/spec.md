# Runtime Specification

## ADDED Requirements

### Requirement: Service Host
The system SHALL run its privileged part as a Windows service named `MyClean` (`myclean-svc.exe`) under LocalSystem, with automatic start and an SCM recovery action that restarts it within 5 s after a failure, and SHALL keep crash dumps of its own processes.

#### Scenario: Restart after a crash
Given the `MyClean` service is running
When the service process terminates unexpectedly
Then the SCM restarts it within 5 s and a crash dump of the failed process is kept under `%ProgramData%\MyClean`

#### Scenario: Automatic start at boot
Given the service is registered
When Windows boots
Then the `MyClean` service starts without a user logon

### Requirement: Clean Stop
The service MUST, on an SCM stop request, flush pending database writes and report `SERVICE_STOPPED` within 30 s (deploy contract C3).

#### Scenario: Stop during sampling
Given the service has 8 s of batched samples not yet written
When the SCM sends a stop request
Then the batched samples are written to the database and the service reports `SERVICE_STOPPED` within 30 s

### Requirement: Fail Fast on Startup
The service MUST crash or stop when any startup step fails, and MUST NOT keep running in a partially initialized state (deploy contract C7).

#### Scenario: Pipe cannot be created
Given another process already owns `\\.\pipe\myclean`
When the service starts
Then the service stops with an error logged within the first 5 s instead of running without IPC

### Requirement: Data and State Locations
The service SHALL read its data files from `..\data` relative to its own executable, SHALL keep all state under `%ProgramData%\MyClean`, and MUST NOT write into its version folder (deploy contract C4).

#### Scenario: Running from a versioned folder
Given the service runs from `C:\Program Files\MyClean\versions\<version>\bin\myclean-svc.exe`
When it runs for 24 h
Then it has loaded `versions\<version>\data` and no file in `versions\<version>` was created or modified

### Requirement: Local Store
The service SHALL keep its store in SQLite in WAL mode at `%ProgramData%\MyClean\myclean.db`, writable only by SYSTEM and Administrators and readable by the owner, and SHALL run an integrity check on the database at every start.

#### Scenario: Integrity check at start
Given the database exists from a previous run
When the service starts
Then an integrity check runs before collectors write to it and its result is logged

#### Scenario: Standard user cannot write the store
Given the owner runs a non-elevated process
When that process opens `myclean.db` for writing
Then access is denied

### Requirement: IPC Pipe
The service SHALL expose `\\.\pipe\myclean` in message mode carrying length-prefixed JSON-RPC 2.0, with a DACL that grants access only to SYSTEM and the owner's SID.

#### Scenario: Owner connects
Given the service is running
When `myclean.exe` running as the owner connects to the pipe and calls `metrics.query`
Then it receives a JSON-RPC 2.0 response

#### Scenario: Other account denied
Given the service is running
When a process running as a different local user opens `\\.\pipe\myclean`
Then the open fails with access denied

### Requirement: Typed Methods Only
The service MUST accept only named, typed IPC methods and MUST reject any request that carries a raw file path or command line, without executing anything.

#### Scenario: Unknown method
Given a connected client
When it calls a method the service does not define
Then the service answers with JSON-RPC error -32601 and performs no work

#### Scenario: Raw path smuggled in parameters
Given a connected client
When it calls a read method with an extra parameter containing a file path or a command line
Then the service answers with JSON-RPC error -32602 and nothing is read from that path or executed

### Requirement: Tray Process Lifecycle
The tray application (`myclean-tray.exe`) SHALL run with the owner's token, start at logon through HKCU `Run`, allow a single instance per session, and exit cleanly when the named event `Local\MyClean.Tray.Quit` is signaled (deploy contract C5).

#### Scenario: Second instance
Given the tray is running in the owner's session
When `myclean-tray.exe` is started again in the same session
Then the new process exits and the first instance keeps running

#### Scenario: Quit event
Given the tray is running
When `Local\MyClean.Tray.Quit` is signaled
Then the tray removes its icon and exits within 10 s without being killed

### Requirement: Degraded Mode Without the Service
The tray and the CLI SHALL keep working read-only when the service is unreachable, and SHALL state which features are unavailable.

#### Scenario: Service stopped
Given the `MyClean` service is stopped
When the owner opens the tray menu or runs a CLI read command
Then per-user data that does not need the service is still shown and a message names the unavailable features (live telemetry, history, disk health, alerts)

### Requirement: Resource Budget
The service and the tray together MUST stay below 1 % average CPU and 150 MB total RAM (NFR-PERF-1), and MUST NOT issue WMI queries in any 1 s loop.

#### Scenario: 24-hour soak
Given the service and tray run with all M1 collectors on the reference machine
When their CPU and memory are measured over 24 h
Then average CPU is below 1 % of the machine and combined working set stays below 150 MB

#### Scenario: No WMI at 1 s cadence
Given all 1 s collectors are running
When WMI activity from MyClean processes is traced for 60 s
Then no WMI query is issued at a 1 s cadence
