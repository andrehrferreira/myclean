# 03 — Resource governor

The tray daemon's job is to keep the machine usable when a build, a test run,
or any other process tries to take all the CPU or RAM, without ever touching
a game the owner is playing.

## Design in one paragraph

Known build/test toolchains are put inside a **Windows Job Object** with hard
caps **the moment they start**, so they can never take the whole machine.
Everything else is watched. When the system is under pressure for a sustained
period, the heaviest process tree goes through an **escalation ladder**:
lower priority → efficiency mode → CPU/affinity cap → suspend → kill, with
kill opt-in only. Games, system processes, the foreground window and
anything on the owner's allow-list are **protected** and never throttled.

## Findings

### F-019 — Reacting after the spike is too late; cap build trees at launch
- **Evidence:** Owner report: builds and test runs "explode" CPU and memory.
  Once the commit charge nears its limit, Windows pages heavily and the
  daemon itself becomes slow to respond.
- **Impact:** The governor watches process creation. When a new process
  matches a `build`/`test` class (table below), it is assigned to that
  class's job object within milliseconds. Its children inherit the job
  automatically, so a `cargo` → `rustc` ×32 tree or an `msbuild` → `cl.exe` ×32
  tree stays inside the cap from the start.
- **Confidence:** high.

### F-020 — Job objects provide the hard caps we need, including soft warnings
- **Evidence:** Win32 job object API (Windows 8+ supports nested jobs, so
  processes already in a job such as Visual Studio's can still be assigned):
  - `JobObjectCpuRateControlInformation` with
    `JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP`.
    `CpuRate` is in 1/100 of a percent of the whole machine (7500 = 75 %).
  - `JobObjectExtendedLimitInformation` with `JOB_OBJECT_LIMIT_JOB_MEMORY`
    caps the committed memory of the whole tree. Over the cap, allocations
    fail and the build errors out, instead of the machine thrashing.
  - `JobObjectNotificationLimitInformation` sets **soft** limits that only
    post `JOB_OBJECT_MSG_NOTIFICATION_LIMIT` to an I/O completion port. These
    give early warnings at 80 % of the hard cap.
  - The same completion port reports `JOB_OBJECT_MSG_NEW_PROCESS` /
    `EXIT_PROCESS`, which gives exact tree membership for free.
  - Assigning a process needs `PROCESS_SET_QUOTA | PROCESS_TERMINATE`; for
    elevated targets this requires the service (SYSTEM) context.
- **Impact:** Default caps for the `build` class on this machine: CPU 75 %
  (≈24 of 32 threads), job commit 96 GB (RAM minus 24 GB headroom for the
  desktop, IDE and browser). Both are configurable per class.
- **Confidence:** high. Verify the API details against current docs in the
  phase-4 spike, per the "never guess API behavior" rule.

### F-021 — Unknown offenders go through a reversible escalation ladder
- **Evidence:** Not every runaway is a known toolchain (owner: "or any other").
- **Impact:** The pressure loop samples once per second. **Pressure** means
  any of: total CPU > 90 % for 15 s; available RAM < 8 GB; commit charge
  > 90 % of the limit. Under pressure, the loop takes the heaviest
  non-protected **tree** (CPU and memory are summed over a root and its
  descendants) and applies the next step, waiting a 10 s cooldown before the
  following one:
  1. priority class → `BELOW_NORMAL`, memory priority → `MEMORY_PRIORITY_LOW`
     (`SetProcessInformation(ProcessMemoryPriority)`);
  2. efficiency mode (EcoQoS):
     `SetProcessInformation(ProcessPowerThrottling, PROCESS_POWER_THROTTLING_EXECUTION_SPEED)`;
  3. assign to a "quarantine" job with a CPU hard cap (50 %) and a memory cap
     set to current usage plus 10 %;
  4. **suspend** the tree, with a tray notification offering
     Resume / Kill / Always allow;
  5. **kill**, but only if the owner enabled kill for that class.
     Never by default.

  Steps 1–4 are undone automatically once pressure has been gone for 60 s
  (hysteresis), except a suspend, which waits for the owner. Every step is
  logged with the reason and the metrics at that moment.
- **Confidence:** high for the mechanism. Thresholds get tuned with real data.
  Suspend uses `NtSuspendProcess`, which is undocumented but stable and widely
  used. The spike confirms its behavior on Windows 11 26200.

### F-022 — Process-start detection must be event-driven
- **Evidence:** Polling once per second misses short-lived compilers, and a
  child spawned before its parent is caught escapes the job.
- **Impact:** The service subscribes to the ETW provider
  `Microsoft-Windows-Kernel-Process` (process start/stop, with image path,
  command line and parent PID) through **krabsetw**, Microsoft's C++ ETW
  library. A one-second `NtQuerySystemInformation(SystemProcessInformation)`
  sweep is the fallback and the source of CPU/memory figures; one call
  returns every process's times, working set and private bytes. Parent links
  are validated by creation time to defeat PID reuse.
- **Confidence:** high.

### F-023 — Games are detected from several signals and never throttled
- **Evidence:** Owner: games may look like a runaway build. E: is labelled
  "Tools & Games".
- **Impact:** A process is a **game** if any of these holds:
  1. it is on the owner's game list;
  2. its image lives under a known library root (`steamapps\common`, Epic,
     GOG, Xbox `WindowsApps`/`XboxGames`, plus owner-defined roots such as
     `E:\Games`);
  3. it is registered in Game Bar's store (`HKCU\System\GameConfigStore`;
     to be verified in the spike);
  4. it is the foreground window while `SHQueryUserNotificationState` returns
     `QUNS_RUNNING_D3D_FULL_SCREEN` or `QUNS_BUSY`, **and** it shows sustained
     GPU use (PDH `\GPU Engine(pid_*)\Utilization Percentage`).

  While a game runs, the governor switches to **game mode**: the game and its
  children are protected, build jobs get a tighter cap (default 40 % CPU),
  and the ladder becomes stricter for everything else. A tray toggle
  ("Game mode: auto / on / off") overrides detection.
- **Confidence:** medium for signals 3–4, which need validation on real
  titles. High for 1–2.

### F-024 — The 7950X3D's two CCDs allow a better split than a plain cap
- **Evidence:** Machine snapshot in file 01: Ryzen 9 7950X3D. One CCD has the 96 MB
  3D V-Cache (best for games), the other has higher clocks (best for
  compilers).
- **Impact:** Optional policy, off by default: in game mode, set the build
  job's CPU set (`SetProcessDefaultCpuSets`, or affinity on the job) to the
  logical processors of the non-V-Cache CCD. The game keeps the cache CCD to
  itself. CCD membership is detected at runtime with
  `GetLogicalProcessorInformationEx(RelationCache)`, grouping processors by
  L3 size. Nothing is hard-coded.
- **Confidence:** medium. This interacts with AMD's own core-parking driver
  and must be measured, not assumed.

### F-025 — The governor must cost almost nothing
- **Evidence:** A monitor that slows the machine defeats its purpose.
- **Impact:** Budgets: < 0.5 % average CPU, < 60 MB RAM for the service.
  One snapshot syscall per second, ETW for events, no WMI in the hot path,
  and SQLite writes batched every 10 s (per-minute aggregates kept 30 days,
  per-hour kept 1 year). The service measures itself and publishes these
  numbers in the tray.
- **Confidence:** high.

## Default process classes

Classes are data (config file), not code. First match wins.

| Class | Examples (image, or image + command-line pattern) | Policy |
|---|---|---|
| `system` | `csrss`, `dwm`, `winlogon`, `lsass`, `svchost`, `audiodg`, MyClean itself | protected |
| `game` | F-023 | protected; enables game mode |
| `allow` | owner's list | protected |
| `build` | `cl`, `link`, `lld-link`, `clang*`, `msbuild`, `ninja`, `cmake`, `make`, `rustc`, `cargo`, `go` (build), `javac`, `java` + `GradleDaemon`, `dotnet` + `build`, `UnrealBuildTool`, `ShaderCompileWorker`, `node` + `webpack`/`vite`/`tsc`/`esbuild`/`next build` | job at launch: CPU 75 %, commit 96 GB |
| `test` | `node` + `jest`/`vitest`/`playwright`, `python` + `pytest`, `dotnet` + `test`, `cargo` + `test` | job at launch: CPU 75 %, commit 64 GB |
| `default` | everything else | watched; escalation ladder under pressure |

## Tray behavior

- The icon color reflects state: normal / under pressure / game mode / an
  action is waiting for the owner.
- The tooltip shows CPU, RAM, commit and C: free space.
- Notifications are rate-limited (at most one per tree per 5 min) and carry
  actions: *Resume*, *Kill*, *Allow this time*, *Always allow*.
- A "Pause governor for 1 h" item.
