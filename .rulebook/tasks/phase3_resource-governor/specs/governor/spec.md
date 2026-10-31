# Governor Specification

## ADDED Requirements

### Requirement: Process Classification
The governor MUST classify every process at start against the classes in `data/classes.toml` (`system`, `game`, `allow`, `build`, `test`, `default`), matching on image name, command-line pattern, path root and signer, where the first matching class wins and an unmatched process is `default`.

#### Scenario: Build tool classified by image
Given `classes.toml` lists `cl` and `rustc` in class `build`
When `cl.exe` starts
Then it is classified `build`

#### Scenario: Command line decides the class
Given `node` with `jest` is in class `test` and `node` with `vite` is in class `build`
When `node.exe ...\jest.js --runInBand` starts
Then it is classified `test`

#### Scenario: First match wins
Given `MyClean` images are listed in class `system` above a `build` rule that would also match them
When a MyClean process starts
Then it is classified `system`

#### Scenario: Invalid class file keeps previous classes
Given a running governor with a valid class set
When `classes.toml` is replaced by a file with a malformed regular expression and reloaded
Then the file is rejected whole and the previous classes stay in effect

### Requirement: Protected Processes Receive No Action
The governor MUST NOT apply any ladder step, job assignment, suspend or kill to a process in class `system`, `game` or `allow`, to the foreground window's process, or to any descendant of a protected `game` process.

#### Scenario: Game never receives an action
Given a running process classified `game` using 95 % CPU while the system is under pressure
When the ladder selects a tree to act on
Then the game's tree is never selected
And `governor_event` and `action_log` contain no entry targeting the game or its children

#### Scenario: System process ignored
Given `svchost.exe` is the largest CPU consumer during pressure
When the ladder selects a tree
Then the `svchost.exe` tree is skipped and the heaviest non-protected tree is selected instead

### Requirement: Proactive Build and Test Jobs
When a process classified `build` or `test` starts, the governor MUST assign it, on the ETW process-start event, to the named job object `Global\MyClean.Job.<class>` configured with a CPU hard cap (`JobObjectCpuRateControlInformation` with hard cap; default 75 % of the machine) and a whole-tree commit cap (`JOB_OBJECT_LIMIT_JOB_MEMORY`; default 96 GB for `build`, 64 GB for `test`), plus notification limits at 80 % of each hard cap. Both caps SHALL be configurable per class.

#### Scenario: Build tree capped from launch
Given default caps
When `cargo build` starts and spawns 32 `rustc` processes
Then `cargo` and every `rustc` are members of `Global\MyClean.Job.build`
And the job's total CPU never exceeds 75 % of the machine

#### Scenario: Commit cap stops the tree, not the machine
Given a `test` tree in `Global\MyClean.Job.test` whose allocations approach 64 GB committed
When it tries to commit beyond 64 GB
Then the allocation fails inside the test tree
And system commit charge stays below the system commit limit

#### Scenario: Early warning at 80 %
Given a `build` job with a 96 GB commit cap
When the job's committed memory passes 76.8 GB
Then a notification-limit event is recorded in `governor_event` for that job

#### Scenario: Process already in an IDE job
Given `msbuild.exe` started by Visual Studio inside Visual Studio's own job
When the governor assigns it to `Global\MyClean.Job.build`
Then the assignment succeeds as a nested job and the build cap applies

### Requirement: Tree Membership Tracking
The governor MUST track job tree membership from the job completion port's new-process and exit-process messages, and MUST validate parent links by process creation time so that a reused PID is never treated as a member of an older tree.

#### Scenario: Child membership from the completion port
Given a `build` job
When `ninja.exe` in the job spawns `cl.exe`
Then the governor records `cl.exe` as a member of the `build` tree

#### Scenario: PID reuse not mistaken for a child
Given a process with PID 4120 created after its claimed parent exited
When the governor builds process trees
Then PID 4120 is not attached to the exited parent's tree

### Requirement: Pressure Detection
The governor MUST evaluate pressure once per second from total CPU, available memory, commit charge and hard faults per second, and SHALL enter the pressure state when total CPU stays above 90 % for 15 s, available memory falls below 8 GB, commit charge exceeds 90 % of the commit limit, or hard faults per second exceed the configured threshold.

#### Scenario: Sustained CPU enters pressure
Given total CPU above 90 % for 14 consecutive seconds
When the 15th consecutive second above 90 % is sampled
Then the governor enters the pressure state with reason `cpu`

#### Scenario: Short CPU spike ignored
Given total CPU above 90 % for 10 s that then drops to 60 %
When the samples are evaluated
Then the governor does not enter the pressure state

#### Scenario: Low available memory enters pressure
Given available memory drops to 7.5 GB
When the sample is evaluated
Then the governor enters the pressure state with reason `available_memory`

#### Scenario: Commit near limit enters pressure
Given a commit limit of 150 GB and commit charge reaching 136 GB
When the sample is evaluated
Then the governor enters the pressure state with reason `commit`

### Requirement: Pressure Clear Hysteresis
The governor MUST leave the pressure state only after every pressure signal has stayed below its exit threshold for 60 continuous seconds.

#### Scenario: Pressure clears after 60 s
Given the governor is in the pressure state
When every signal stays below its exit threshold for 60 s
Then the pressure state clears

#### Scenario: Relapse restarts the clear timer
Given every signal has been below its exit threshold for 45 s
When CPU rises above 90 % again for 15 s
Then the pressure state does not clear and the 60 s clear timer restarts once all signals are below their exit thresholds again

### Requirement: Escalation Ladder
While under pressure the governor MUST act on the heaviest non-protected process tree (CPU and memory summed over a root and its descendants), applying one step at a time with at least 10 s between steps on the same tree: (1) priority class `BELOW_NORMAL` and memory priority `MEMORY_PRIORITY_LOW`; (2) EcoQoS execution-speed throttling; (3) assignment to a cap job with a 50 % CPU hard cap and a memory cap at current usage plus 10 %; (4) suspend of the whole tree with an owner notification; (5) kill, only when kill is enabled for the tree's class. Kill SHALL be disabled for every class by default.

#### Scenario: Ladder escalates with cooldown
Given pressure persists and tree T is the heaviest non-protected tree
When the governor acts on T
Then every process in T first receives priority `BELOW_NORMAL` and memory priority `MEMORY_PRIORITY_LOW`
And the next step on T is applied no sooner than 10 s later

#### Scenario: Suspend notifies the owner
Given tree T has reached the cap-job step and pressure persists for another 10 s
When the next step is applied
Then every process in T is suspended
And the owner receives a notification with Resume, Kill, Allow once and Always allow

#### Scenario: Kill off by default
Given default configuration and a suspended tree T while pressure persists
When the ladder would take the next step on T
Then T is not killed and stays suspended

#### Scenario: Kill when opted in
Given the owner enabled kill for class `default`
And a `default`-class tree T is suspended while pressure persists for another 10 s
When the next step is applied
Then T is terminated and the kill is recorded with the reason and metrics

### Requirement: Automatic Rollback
When pressure clears the governor MUST undo priority, memory priority, EcoQoS and cap-job steps on every tree it touched, and MUST NOT resume a suspended tree automatically; a suspended tree SHALL stay suspended until the owner acts on it, the governor is paused or stopped, or the service recovers from a crash.

#### Scenario: Throttled tree restored
Given tree T at priority `BELOW_NORMAL`, memory priority low, EcoQoS on and in a cap job
When pressure clears after the 60 s hysteresis
Then T's original priority class, memory priority and power-throttling state are restored
And T is no longer subject to the 50 % cap job

#### Scenario: Suspended tree waits for the owner
Given tree T suspended by the ladder
When pressure clears
Then T stays suspended and its notification remains actionable

### Requirement: Every Step Recorded
Every governor intervention and every rollback MUST be written to `governor_event` and, as a `GovernorStep` action with actor `governor` (or `owner` for tray actions), to `action_log`, including the tree root, the step, the reason and the pressure metrics at that moment.

#### Scenario: Ladder step logged
Given the governor applies EcoQoS to tree T under CPU pressure
When the step completes
Then `governor_event` has a row with T's root image and PID, step `ecoqos`, reason `cpu` and the CPU, memory and commit values at that second
And `action_log` has the matching `GovernorStep` entry with actor `governor`

### Requirement: Fail-Open on Clean Stop
On a clean service stop the governor MUST lift every cap it set, remove limits from its named jobs, restore every ladder step and resume every tree it suspended, before the service reports `SERVICE_STOPPED` within 30 s (deploy contract C3).

#### Scenario: Stop releases everything
Given a `build` tree capped at 75 % in `Global\MyClean.Job.build` and a suspended `default` tree
When the SCM stops the `MyClean` service
Then the build tree runs without a CPU or commit cap
And the suspended tree is resumed
And the service reports `SERVICE_STOPPED` within 30 s

### Requirement: Crash Recovery and Job Re-Adoption
Job objects MUST be created without `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, every suspend MUST be persisted before it is applied, and a service restarted after a crash MUST reopen its named jobs `Global\MyClean.Job.<class>`, reconcile their limits with the current policy, and resume every suspended tree it cannot account for.

#### Scenario: Crash does not kill the build
Given a running `build` tree in `Global\MyClean.Job.build`
When the service process is terminated abnormally
Then every process in the build tree keeps running

#### Scenario: Restarted service re-adopts jobs
Given the service crashed while `Global\MyClean.Job.build` held a running tree
When the SCM restarts the service within 5 s
Then the service reopens `Global\MyClean.Job.build` instead of creating a new job
And the job's limits match the current class policy

#### Scenario: Orphaned suspend resumed
Given a tree suspended by the governor whose persisted suspend record does not match the state after restart
When the restarted service reconciles
Then the tree is resumed and a `governor_event` records the resume with reason `orphaned_suspend`

#### Scenario: No orphan left after kill and restart
Given caps and a suspend are active
When the service is killed and the SCM restarts it
Then after reconciliation no process remains suspended or capped by a limit the current policy does not call for

### Requirement: Game Detection
The governor MUST classify a process as a game when it is on the owner's game list, its image lives under a known library root (`steamapps\common`, Epic, GOG, Xbox `WindowsApps` or `XboxGames`, or an owner-defined root such as `E:\Games`), it is registered in `HKCU\System\GameConfigStore`, or it owns the foreground window while `SHQueryUserNotificationState` reports `QUNS_RUNNING_D3D_FULL_SCREEN` or `QUNS_BUSY` and it shows sustained GPU use.

#### Scenario: Game detected from library root
Given `E:\Games` is an owner-defined library root
When `E:\Games\Title\title.exe` starts
Then it is classified `game`

#### Scenario: Fullscreen with GPU use
Given an unlisted process owns the foreground window in exclusive fullscreen with sustained GPU engine utilization
When detection runs
Then the process is classified `game`

#### Scenario: Fullscreen without GPU use
Given a fullscreen video player with negligible GPU engine utilization and no other game signal
When detection runs
Then the process is not classified `game`

### Requirement: Game Mode
While a game runs (or the tray override is `on`) the governor MUST enter game mode, which protects the game and its descendants, lowers the `build` job CPU cap to 40 % by default and applies the stricter game-mode ladder to every other non-protected tree; the tray override `auto` / `on` / `off` SHALL take precedence over detection, and a detected game SHALL remain protected in every override state.

#### Scenario: Game starts during a build
Given a `build` tree capped at 75 %
When a game is detected
Then the `build` job's CPU cap becomes 40 %
And it returns to 75 % when the last game exits

#### Scenario: Override off
Given the game-mode override is `off`
When a game is detected
Then the build cap stays at 75 %
And the game still receives no governor action

#### Scenario: Override on
Given the override is `on` and no game is running
When the governor evaluates its mode
Then game mode is active and the build cap is 40 %

### Requirement: Rate-Limited Owner Notifications
The tray MUST notify the owner of governor interventions at most once per tree per 5 min, and each notification SHALL offer Resume, Kill, Allow once and Always allow, where Always allow adds the tree's image to the owner's `allow` class persistently and every choice is recorded with actor `owner`.

#### Scenario: Repeated interventions rate-limited
Given the governor applies three steps to tree T within 2 min
When the tray processes the interventions
Then the owner receives one notification for T

#### Scenario: New notification after 5 min
Given the last notification for T was 5 min ago
When the governor applies a new step to T
Then a new notification for T is shown

#### Scenario: Always allow
Given a notification for tree T with root image `bigjob.exe`
When the owner chooses Always allow
Then `bigjob.exe` is added to the `allow` class and T's applied steps are rolled back
And future `bigjob.exe` processes receive no governor action

### Requirement: Owner Exemptions Survive Continued Pressure
When the owner chooses Allow once for a tree, the governor MUST roll back that tree's applied steps and SHALL NOT apply any ladder step to it again until its root process exits, even while pressure continues. When the owner chooses Resume on a suspended tree, the governor MUST resume it and SHALL NOT suspend it again for 10 min; lower ladder steps may still apply.

#### Scenario: Allow once holds under pressure
Given tree T was suspended and the owner chose Allow once
When pressure continues and T is still the heaviest tree
Then T receives no ladder step and the ladder moves to the next heaviest non-protected tree

#### Scenario: Allow once ends with the tree
Given the owner chose Allow once for tree T and T's root process exited
When a new process with the same image starts and becomes the heaviest tree under pressure
Then the ladder applies to the new tree normally

#### Scenario: Resume blocks re-suspension for 10 minutes
Given the owner resumed suspended tree T at 12:00 and pressure continues
When T is the heaviest tree at 12:05
Then T may receive priority, EcoQoS or cap steps but is not suspended before 12:10

### Requirement: Pause Governor
The owner MUST be able to pause the governor for N minutes from the tray; while paused the governor SHALL apply no ladder step, SHALL roll back steps it already applied and resume trees it suspended, and SHALL resume normal operation automatically when the pause expires.

#### Scenario: Pause for 60 minutes
Given a tree T at the EcoQoS step and another suspended tree U
When the owner pauses the governor for 60 min
Then T's steps are rolled back and U is resumed
And no ladder step is applied for 60 min even under pressure

#### Scenario: Pause expires
Given the governor was paused for 60 min
When 60 min have elapsed
Then the governor resumes pressure evaluation and the ladder

### Requirement: Governor Overhead and Effect
The governor MUST keep the service and tray together within NFR-PERF-1 (< 1 % average CPU, < 150 MB RAM) with no WMI query in its 1 s loop, and under the stress scenario (parallel C++ and Rust builds plus a memory-hog test) MUST keep foreground probe latency below 100 ms with no commit exhaustion.

#### Scenario: Stress scenario
Given parallel C++ and Rust builds and a memory-hog test started together
When they run to completion under the governor with default caps
Then every foreground probe latency sample stays below 100 ms
And system commit charge never reaches the commit limit

#### Scenario: Idle overhead
Given the service and tray running for 24 h with the governor active
When their resource use is averaged
Then average CPU is below 1 % and combined RAM is below 150 MB
