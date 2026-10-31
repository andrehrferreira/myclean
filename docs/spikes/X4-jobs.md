# X4 — Job objects and suspend (spike note)

**Status:** verified, non-elevated. SYSTEM-only items are flagged. Run on
2026-10-31 on Windows 11 Pro 10.0.26200, Ryzen 9 7950X3D (32 logical CPUs),
127 GB RAM, interactive session 1, not admin.
**Unblocks:** G1, G2. **Requirements touched:** GOV-2, GOV-4, GOV-5,
NFR-REL-2 and the `governor` spec (Proactive Build and Test Jobs, Tree
Membership Tracking, Escalation Ladder, Fail-Open on Clean Stop, Crash
Recovery and Job Re-Adoption, Game Mode, Governor Overhead and Effect).

> **Measurement backdrop.** Other agents' builds and linters kept the machine
> at **98–100 % total CPU** for the whole spike. To spare the owner, who was
> using the PC, every test workload ran at `BELOW_NORMAL_PRIORITY_CLASS`; the
> measuring thread ran at `THREAD_PRIORITY_HIGHEST`. An uncapped 32-thread
> workload therefore got only about 52–60 % of the machine. Caps **below**
> that share (4000, 2000, 1000) are measured cleanly. The **upper bound of the
> 7500 cap could not be exercised** and needs a re-run on an idle machine.

## 1. Questions

- **(a)** Actual CPU of a 32-thread busy workload under
  `JobObjectCpuRateControlInformation` with `HARD_CAP` at `CpuRate` 7500 and 4000.
- **(b)** `JOB_OBJECT_LIMIT_JOB_MEMORY`: what a process that exceeds the job
  commit cap sees (C++ `new`, `malloc`, `VirtualAlloc`, exit code), and
  whether `JobObjectNotificationLimitInformation` soft limits post
  `JOB_OBJECT_MSG_NOTIFICATION_LIMIT`.
- **(c)** Nesting: can a process that is already in a job be assigned to a
  second job on 26200?
- **(d)** Children inherit the job; `JOB_OBJECT_MSG_NEW_PROCESS` /
  `EXIT_PROCESS` give tree membership.
- **(e)** Named jobs: can a second process reopen a `Local\…` job by name
  after the creator exits while members still run, and do the limits
  persist? This is the crash re-adoption design.
- **(f)** `NtSuspendProcess` / `NtResumeProcess` on a tree, and the fate of a
  suspended process when the suspender exits.
- **(g)** Real builds: a parallel `cl.exe` build of a few hundred files, and
  Cargo/Gradle if installed, capped vs uncapped.

## 2. Method

The experimental code lives outside the repo, in
`C:\Users\<user>\AppData\Local\Temp\claude\f--MyClean\c9ddaf7d-862d-42f1-a36e-af1fa4e3176b\scratchpad\spikes\`:

- `x4.cpp`: one program with subcommands; it also acts as its own child
  workloads.
- `build.ps1`: enters the VS 2022 Build Tools x64 dev shell and runs
  `cl /nologo /EHsc /O2 /std:c++20 /W3 /DUNICODE /D_UNICODE <src> <defines> /link user32.lib kernel32.lib ntdll.lib advapi32.lib psapi.lib`.
- `runbuilds.ps1`: drives (g).
- Raw outputs: `x4-*.txt`.

Builds used:

```text
powershell -NoProfile -File build.ps1 x4.cpp -DNO_SUSPEND  -Fejt3.exe -Fojt3.obj   # (a)-(e), (g)
powershell -NoProfile -File build.ps1 x4.cpp -DDYN_SUSPEND -Fejs2.exe -Fojs2.obj   # (f): Nt* via GetProcAddress
```

| Command | What it does |
|---|---|
| `jt3 cpu 0 7500 4000 2000 1000` | For each rate: new job, `CpuRate` with `ENABLE\|HARD_CAP` (none for 0), spawn `x4 burn 32 12` (32 spinning threads) suspended, assign, resume, wait 1.5 s. Then measure job CPU from `JobObjectBasicAccountingInformation` (user + kernel) over 8 s in 250 ms samples and over 2 s in 20 ms samples, as % of the machine (÷ 32 logical CPUs). Then a live change 7500 → 4000 → lift on a running job, and a 4-thread job under 7500 |
| `jt2 cputrace 2000` / `4000` | Job CPU in about 15 ms bins (default `Sleep` granularity) for 1.5 s, segmented into run and stopped phases |
| `jt2 mem <kind> 1024 800 64` | Job with `JobMemoryLimit` 1024 MB, notification limit 800 MB, completion port. The child allocates 64 MB chunks with `new` (catches `bad_alloc`), `newnocatch`, `malloc`, or `VirtualAlloc(MEM_COMMIT)`; at the cap it also tries `CreateThread` and a 4 KB `VirtualAlloc`. Also `reserve` (reserve 4 GB, then commit it) and `osc` (commit 1 GB and free it, three times, under a 2 GB cap) |
| `jt2 nest` | Nesting cases A–E (§3.3) |
| `jt2 treetest` | `x4 tree 2 3 2 1` (root → 3 children → 9 grandchildren) in a job with a port; port associated after a member already exists; second port association; `CREATE_BREAKAWAY_FROM_JOB` from inside a job |
| `jt3 named-create [keeper]` then `jt3 named-open <memberPid>` | The creator makes `Local\MyCleanSpike.Job.build` (cap 4000, commit 2048 MB, notification 1600 MB, port), adds a 32-thread member, and exits without cleanup. With `keeper`, the job handle is inheritable and a sleeping child process holds it. The re-opener checks the member, `OpenJobObject`, the persisted limits, port re-association, reconcile to 7500, and lifting every limit. The creator also tries `CreateJobObject("Global\\…")` |
| `js2 suspendtest` | Tree of root + 3 children × 2 busy threads in an unlimited job (for accounting). `NtSuspendProcess` on each member ×1 and ×2, resume ×1 and ×2. A helper process suspends and exits; another process resumes. `NtResumeProcess` on someone else's `CREATE_SUSPENDED` process. Undocumented job freeze (`NtSetInformationJobObject`, class 18, `JOBOBJECT_FREEZE_INFORMATION`: `Flags=1`, `Freeze=TRUE/FALSE`), including a member added after the freeze, a freezer that exits, and `NtResumeProcess` on frozen members |
| `runbuilds.ps1 -Which cpp\|rust -Configs "rate:commitMB,…"` | `jt3 runjob <rate> <commitMB> <80 % notif>` around `cl /nologo /c /MP32 /EHsc /O2 /std:c++17 *.cpp` (320 generated files, each with `<regex>`, `<map>`, `<unordered_map>`, `<sstream>`, `<iostream>` and unique templates), or `cargo build --offline -j 32 --release` on a generated workspace of 32 independent crates (60 generic structs each, no dependencies). Records wall time, job CPU, total processes, `PeakJobMemoryUsed` and port messages; outputs are cleaned before every run |

`cargo` is on PATH (`C:\Users\<user>\.cargo\bin\cargo.exe`); **`gradle` is
not**, so Gradle was not tested.

## 3. Results

### 3.1 (a) CPU hard cap

32 busy threads, % of the whole machine (100 % = 32 logical CPUs):

| `CpuRate` | 8 s average (250 ms samples) | 2 s average (20 ms samples) | 20 ms min / max | Whole-system busy |
|---|---|---|---|---|
| none | 53.9 % (17.3 CPUs) | 51.8 % | 29.6 / 62.1 | 100 % |
| 7500 | 48.5 % | 50.0 % | 21.0 / 64.4 | 100 % |
| 4000 | **38.9 %** | **40.6 %** | 0.0 / 73.6 | 99–100 % |
| 2000 | **20.4 %** | **22.0 %** | 0.0 / 72.0 | 87–92 % |
| 1000 | **9.7 %** | **9.2 %** | 0.0 / 64.9 | 70–82 % |

An earlier run (before the measuring thread's priority was fixed) gave 51.7 %
at 7500 and 37.7–39.1 % at 4000.

**Enforcement is a duty cycle, not a smooth ceiling** (`x4-cputrace-*.txt`):

| `CpuRate` | Run phases | Fully stopped phases | Cycle |
|---|---|---|---|
| 2000 | 186, 274, 279 ms | 246, 312 ms | about 0.55–0.6 s |
| 4000 | 462–493 ms | 30–106 ms | about 0.55–0.6 s |

During a run phase the job takes its full uncapped share; during a stopped
phase **every thread in the job gets no CPU at all**.

Other results:
- **Live change:** `SetInformationJobObject` on a running job took 0.58–0.85 ms.
  7500 → 4000 took full effect within one cycle: 47.6 % over the first
  500 ms, then 40.2 %. Lifting the cap (`ControlFlags = 0`) restored 58 %
  in the first 500 ms.
- **Light job:** 4 threads under a 7500 cap used 3.76 CPUs, so the cap does
  not slow a job below it.

### 3.2 (b) Job commit cap and notification limit

Cap 1024 MB, notification at 800 MB, 64 MB chunks:

| Child | What the child saw | Exit code | Port |
|---|---|---|---|
| `new` + `catch` | `std::bad_alloc` after 960 MB | 10 (own) | `JOB_MEMORY_LIMIT` with the **child's PID**; `NOTIFICATION_LIMIT` ×2 |
| `new`, uncaught | process dies | **0xC0000409** (`STATUS_STACK_BUFFER_OVERRUN`: UCRT `abort` → fail-fast), about 290 ms later | `JOB_MEMORY_LIMIT`, `NOTIFICATION_LIMIT`, then **`EXIT_PROCESS`, not `ABNORMAL_EXIT_PROCESS`** |
| `malloc` | `NULL` after 960 MB, `errno` 12 (`ENOMEM`), `GetLastError` 8 (`ERROR_NOT_ENOUGH_MEMORY`) | 11 (own) | same pair |
| `VirtualAlloc(MEM_COMMIT)` | `NULL` after 960 MB, **`GetLastError` 1455 (`ERROR_COMMITMENT_LIMIT`)**. In the remaining 64 MB of headroom, `CreateThread` and a 4 KB commit still **succeeded** | 12 (own) | `JOB_MEMORY_LIMIT`; `NOTIFICATION_LIMIT` ×3 |
| `reserve` | `MEM_RESERVE` of 4096 MB **succeeds** (reserve is not charged); committing it fails with 1455 | 0 | `JOB_MEMORY_LIMIT`. `PeakJobMemoryUsed` then reported **4096 MB**, above the 1024 MB cap |
| `osc` (cap 2048 MB) | 3 rounds of commit 1 GB, free, wait | 0 | **one `NOTIFICATION_LIMIT` per crossing** (at +26 ms, +1462 ms, +2885 ms) |

Port and limit-violation details:
- Every `NOTIFICATION_LIMIT` carries **PID 0**.
- `JobObjectLimitViolationInformation` returns `ViolationLimitFlags = 0x200`
  (`JOB_MEMORY`), `JobMemory` (962 or 1026 MB at query time) and
  `JobMemoryLimit` 800 MB.
- In fast allocation loops the notification arrived about 1 ms *after* the
  hard-limit message, even though 800 MB was crossed earlier. Do not rely on
  the order of the two messages.

### 3.3 (c) Nesting

| Case | Steps | Result |
|---|---|---|
| A | P in an unlimited "IDE" job → assigned to a build job with `CpuRate` 4000 | **OK**. P is in both jobs; effective CPU 39.1 % |
| B | Parent job capped at 4000; P → nested child job, then child capped at 7500 | OK; effective **28.9 %**: the caps compound to *below* the tighter one (single run, under contention) |
| C | One shared build job. Assign P1 (in IDE job A), then P2 (in IDE job B), then P3 (in no job) | P1 OK; **P2 fails, error 5 (`ERROR_ACCESS_DENIED`)**; P3 OK, **but P3 is now also a member of IDE job A** |
| D | Shared build job. Assign P3 (in no job) first, then P1 (in IDE job A) | P3 OK; **P1 fails, error 5** |
| E | P1 in IDE A → build job → another empty job | Both OK (chain A ⊃ build ⊃ other) |

A job joins the hierarchy of the first member that is already in a job.
After that it can only take processes from that hierarchy or from no job, and
it drags the job-free ones *into* that hierarchy.

### 3.4 (d) Inheritance and tree membership

- **Tree of 13** (root → 3 → 9, created normally by the members):
  `JobObjectBasicProcessIdList` listed 13 while running. The port delivered
  13 `NEW_PROCESS` and 13 `EXIT_PROCESS`, 0 `ABNORMAL_EXIT_PROCESS` and 1
  `ACTIVE_PROCESS_ZERO`; `TotalProcesses` = 13.
- **Port associated after a member already existed:** Windows posted a
  `NEW_PROCESS` for the existing member at association time (+36 ms), then
  its `EXIT_PROCESS` and `ACTIVE_PROCESS_ZERO`.
- **Second port association on the same job:** fails, **error 87**
  (`ERROR_INVALID_PARAMETER`). A job can have only one completion port, ever.
- **Breakaway:** a member calling `CreateProcess(CREATE_BREAKAWAY_FROM_JOB)`
  in a job without `JOB_OBJECT_LIMIT_BREAKAWAY_OK` fails with **error 5**.

### 3.5 (e) Named jobs and crash re-adoption

| Step | Without a handle holder | With a keeper process holding an inherited handle |
|---|---|---|
| Creator exits without cleanup; member (32 threads) still running | member running, in a job | member running, in a job |
| `OpenJobObject("Local\\MyCleanSpike.Job.build")` | **fails, error 2 (`ERROR_FILE_NOT_FOUND`)** | **OK** |
| Member CPU after the creator's death | **39.0 %, still capped at 4000**, measured through the process handle. The job is unreachable by any API. An earlier run showed the cap held for the member's whole 40 s life (509.8 CPU-s = 12.7 CPUs) | 39.1 % (cap persisted) |
| Persisted limits as read back | n/a | `CpuRate` flags 0x5, rate 4000; `LimitFlags` 0x200, `JobMemoryLimit` 2048 MB; notification `LimitFlags` 0x200, 1600 MB. All persisted |
| `CreateJobObject` with the same name | n/a | returns the **existing** job; `GetLastError` = 183 (`ERROR_ALREADY_EXISTS`) |
| Associate a new completion port | n/a | **fails, error 87**: the dead creator's association still blocks it |
| Reconcile to 7500, then lift every limit | n/a | both took effect: 48.5 %, then 50.9 % (the uncapped share under contention) |

Global namespace:
- `CreateJobObject("Global\\MyCleanSpike.Job.build")` from this
  **non-admin** session-1 process **succeeded**, although the token has no
  `SeCreateGlobalPrivilege` (`whoami /priv` does not list it). That privilege
  only gates file mappings and symbolic links, not job objects.
- **Not verified (needs SYSTEM):** the service creating `Global\…` jobs, and
  `OBJ_PERMANENT` named jobs (`NtCreateJobObject` with `OBJ_PERMANENT`,
  which needs `SeCreatePermanentPrivilege`; SYSTEM holds it).

### 3.6 (f) `NtSuspendProcess` / `NtResumeProcess` on a tree

| Step (4 processes, 8 busy threads) | Job CPU |
|---|---|
| running | 22.2 % |
| `NtSuspendProcess` ×1 on each (0.034 ms for all four) | **0.0 %** |
| suspended again (count 2), resumed once | **0.0 %**: suspends are counted |
| resumed a second time | 23.2 % |
| helper process suspends all four and **exits** | **0.0 % for 3 s**: the suspend outlives the suspender |
| resumed by a different process | 22.4 % |
| someone else's `CREATE_SUSPENDED` process, then `NtResumeProcess` | 0 % → 5.5 %: **resume is not owner-aware**; it also releases the initial creation suspend |

Undocumented job freeze (`NtSetInformationJobObject`, class 18), non-admin,
on our own job:

| Step | Result |
|---|---|
| freeze / thaw | `STATUS_SUCCESS` both; CPU 0.0 % while frozen, 18–23 % after thaw |
| process **assigned after** the freeze | **not frozen** (1.35 CPUs of its 2 threads) |
| freezer is a helper process (duplicated handle) that **exits** | job **stays frozen** (0.0 % for 2 s) until thawed through another handle |
| `NtResumeProcess` on each frozen member | **still 0.0 %**: freeze is independent of the suspend count |

### 3.7 (g) Real builds

**MSVC, 320 files, `cl /MP32`** (tree: 1 + 32 `cl.exe` = 33 processes),
below-normal priority, machine saturated by other work:

| `CpuRate` | Commit cap | Wall | Job CPU | Average job CPU (% of machine) | `PeakJobMemoryUsed` | Result |
|---|---|---|---|---|---|---|
| none | none | 69.4 s | 891.6 s | 40.1 % | 4718 MB | 320 .obj, exit 0 |
| 7500 | none | 66.4 s | 889.7 s | 41.8 % | 4591 MB | 320 .obj, exit 0 |
| 4000 | none | 72.9 s | 898.3 s | 38.5 % | 4520 MB | 320 .obj, exit 0 |
| 2000 | none | **207.9 s** (3.0×) | 906.0 s | 13.6 % | 4797 MB | 320 .obj, exit 0 |
| none | **2048 MB** | 15.8 s, failed | 119.1 s | 23.5 % | 2048 MB | **26 .obj**; root `cl` exit 2; **287 × `fatal error C1060` (compiler is out of heap space)** plus `D8040` (cl driver could not create or talk to a child); 33 `JOB_MEMORY_LIMIT` (one per `cl.exe`); 1 `ABNORMAL_EXIT_PROCESS`; **13,740 `NOTIFICATION_LIMIT` messages** |

**Cargo, 32 crates, `-j 32 --release`** (cargo + 32 `rustc` + helpers = 37
processes):

| `CpuRate` | Commit cap | Wall | Job CPU | Average job CPU | `PeakJobMemoryUsed` | Result |
|---|---|---|---|---|---|---|
| none | none | 9.6 s | 99.0 s | 32.2 % | 3624 MB | 32 rlibs |
| 4000 | none | 13.7 s (1.42×) | 98.7 s | 22.5 % | 4983 MB | 32 rlibs |
| 2000 | none | 23.3 s (2.4×) | 101.5 s | 13.6 % | 4038 MB | 32 rlibs |
| none | **1024 MB** | 4.9 s, failed | 2.6 s | 1.7 % | 1074 MB | **0 rlibs**. `rustc`: `memory allocation of 524288 bytes failed`; `failed to mmap rmeta metadata` (read-only file mappings fail too); **`cargo` itself died with 0xC0000409**; 22 processes; 17 `JOB_MEMORY_LIMIT`; 189 `NOTIFICATION_LIMIT` |

Notes:
- Job CPU-seconds are the same capped or uncapped (889–906 s for C++, 99–102
  s for Cargo). The cap only stretches the build in time.
- Average job CPU stays below the cap in the 2000 runs because the tail of
  each build has fewer parallel compilers.
- Peak job commit for the same build varied by 10 % between runs
  (4.5–4.8 GB for C++, 3.6–5.0 GB for Cargo). That is about 140–150 MB per
  `cl.exe` and about 110–155 MB per `rustc` for this synthetic code.

### 3.8 Antivirus interference (unplanned finding)

The active antivirus is **Kaspersky** (Security Center lists Kaspersky 21.24
and 21.26; Defender is passive).
- The first `x4.exe` build **statically imported `NtSuspendProcess` /
  `NtResumeProcess`** from `ntdll.lib`. Kaspersky refused to execute it
  ("Access denied" / "Permission denied"), locked it against reads, and then
  **deleted it**. This happened twice.
- Later, two rebuilds *without* those imports were also blocked: one added
  `winmm` / `timeBeginPeriod`, and one was the same source under a new name.
  A rebuild without `winmm` ran. The detection is heuristic and not stable
  across near-identical binaries.
- The `GetProcAddress` variant (`-DDYN_SUSPEND`) ran unhindered, including the
  undocumented job freeze.
- Non-admin, the detection names could not be read.

## 4. Conclusions per question

- **(a)** The hard cap is accurate **on average**: 38.9–40.6 % at 4000,
  20.4–22.0 % at 2000, 9.2–9.7 % at 1000, in a cycle of about 0.6 s. Within
  the cycle the job runs at full speed, then is **completely stopped** for up
  to about 0.3 s. 20 ms windows reach 64–74 % of the machine even under a
  10–40 % cap. Changes on a running job apply within one cycle and cost under
  1 ms. 7500 is honoured as an upper bound in every sample average, but
  whether 7500 is reached on an *idle* machine remains unverified.
- **(b)** Over the job commit cap, allocations fail normally: `bad_alloc`,
  `NULL`/`ENOMEM`, `ERROR_COMMITMENT_LIMIT`. Nothing is killed by Windows. A
  program that does not handle the failure dies with 0xC0000409, reported as
  a normal `EXIT_PROCESS`. `JOB_MEMORY_LIMIT` names the failing process.
  Soft limits work, but `NOTIFICATION_LIMIT` is posted without a PID, at
  least once per upward crossing, and in real builds about **once per
  allocation while above the limit** (13,740 in 16 s); the details come from
  `JobObjectLimitViolationInformation`. `PeakJobMemoryUsed` can exceed the
  cap after a refused commit.
- **(c)** Nesting works on 26200: a process already in a job can be assigned
  to an empty job, or to a job in the same hierarchy. **A single job cannot
  hold processes from two different job hierarchies** (error 5). Nested caps
  compound below the tighter cap.
- **(d)** Yes. Children inherit the job, and the port reports exact
  membership, including members present before the port was attached. One
  port per job, for the job's whole life.
- **(e)** **Re-adoption by name does not work on its own.** The name dies
  with the last handle, while members, and their caps, live on. With any
  surviving handle holder, reopen, read limits, reconcile and lift all work,
  but the completion port cannot be re-attached. `Global\` job names can be
  created by any non-admin user.
- **(f)** `NtSuspendProcess` stops a process completely, stacks
  (count-based), survives the suspender's exit, and can be undone by anyone
  with `PROCESS_SUSPEND_RESUME`, which also releases suspends it did not make.
  The undocumented job freeze is atomic over current members, survives the
  freezer, and is independent of suspend counts.
- **(g)** Real toolchains behave as expected inside a job.
  - **CPU:** `cl /MP32` and `cargo -j 32` trees (33 and 37 processes) are
    fully contained. A binding cap stretches wall time in proportion (2000:
    3.0× for C++, 2.4× for Cargo) at constant CPU-seconds. Caps above the
    share the build gets anyway (7500, 4000 on this saturated machine)
    changed nothing.
  - **Commit cap:** a cap below the tree's peak breaks the build. MSVC
    reports `C1060` per file and keeps compiling other files: 26 of 320
    objects built. `rustc` and `cargo` abort. The machine stays fine.
  - **Notification flood:** the soft limit **floods the completion port**,
    with 13,740 messages in 16 s for MSVC.

## 5. Implications for the product spec and architecture

1. **Proactive Build and Test Jobs: one shared job per class is not
   viable.** Per §3.3 C/D:
   - Once `MyClean.Job.build` holds a process from a Visual Studio job, it
     rejects a `cargo` started from a terminal (error 5), and the other way
     round.
   - Worse, a job-free process assigned later is silently **pulled into the
     IDE's job**. It then inherits that job's limits, including
     `KILL_ON_JOB_CLOSE` if the IDE uses it, so closing the IDE could kill an
     unrelated build.

   Change the design to **one job per tree root**, named for example
   `MyClean.Job.<class>.<rootPid>.<rootCreateTime>`, each carrying the class
   caps.
   - For job-free roots, a per-class *parent* job can still give an
     aggregate cap. Assign the root to the class job first, then to its
     per-tree job, which nests the tree job under the class job (chain
     verified in case E).
   - Roots already in a foreign job get a standalone per-tree job nested
     under that foreign job (case A).
   - The scenario "Process already in an IDE job" passes only with per-tree
     jobs.
   - Also record that **nested caps compound** (case B: 4000 parent + 7500
     child gave 28.9 %). Under an IDE job that already caps CPU, MyClean's
     cap makes things stricter than intended. Read the parent's limits
     before setting ours.
2. **"The job's total CPU never exceeds 75 % of the machine"** holds only
   as an average over ≥ 1 s.
   - Reword the scenario. Measure CPU from job accounting deltas over ≥ 2 s;
     the 1 s pressure loop will alias the 0.6 s sawtooth.
   - **Escalation Ladder step 3** (50 % cap job on an unknown `default`
     tree) will make an interactive app freeze for hundreds of milliseconds
     per cycle. That is acceptable for compilers and visible for UI apps;
     state it in the notification text, or prefer
     `JOB_OBJECT_CPU_RATE_CONTROL_WEIGHT_BASED` or the min/max-rate mode for
     `default` trees (not tested here).
   - **Under saturation the cap is moot.** The capped build already got
     only about 40 % because it ran below normal against other work, and
     75 % / 40 % caps changed its wall time by under 10 %. The cap protects
     the machine from *the build*, not the foreground from *everything
     else*. Keep the build job's priority class below normal as well. A job
     priority limit via `JOB_OBJECT_LIMIT_PRIORITY_CLASS` was not tested.
3. **Commit cap and Early Warning at 80 %:** confirmed.
   - `governor_event` should take the **offending PID from
     `JOB_MEMORY_LIMIT`**; `NOTIFICATION_LIMIT` has PID 0. Query
     `JobObjectLimitViolationInformation` for the value at that moment.
   - **The soft limit is not one message per crossing in real builds.** A
     capped `cl /MP32` build posted **13,740** `NOTIFICATION_LIMIT`
     messages in 16 s: about one per allocation while above the limit.
     Handle the first one, then **clear the notification limit** (set
     `LimitFlags = 0`) and re-arm it from the 1 s loop once usage falls
     below, say, 70 %. Otherwise the governor's port thread spins and
     `governor_event` floods.
   - Do not trust `PeakJobMemoryUsed` after a refused commit; it can exceed
     the cap.
   - Classify a tree member's death by **exit code** (0xC0000409 /
     0xC0000017), because `ABNORMAL_EXIT_PROCESS` is not posted for
     fail-fast exits.
   - A 64 MB margin was enough for `CreateThread` and small allocations at
     the cap, so the tree can still report its own error.
   - **Sizing:** the commit cap ends a build rather than slowing it. `cargo`
     itself died, and MSVC lost 294 of 320 files. The 96 GB / 64 GB defaults
     are far above the 4–5 GB peaks seen here, which is right for a "protect
     the machine" cap. Never derive a cap from a recent peak; a 10 % margin
     over the peak (ladder step 3: "current usage plus 10 %") would have
     broken these builds, whose peak varied by 10 % between identical runs.
     Give step 3 at least 25 % headroom, or use only the notification limit
     there.
4. **Tree Membership Tracking:** the port gives exact membership, and
   attaching it late synthesizes `NEW_PROCESS` for existing members.
   Because of the one-port-per-job rule, a job **re-adopted after a crash
   gets no port messages**. For those, poll `JobObjectBasicProcessIdList`
   in the 1 s loop until the job drains, and give new trees fresh jobs with
   ports. Never set `BREAKAWAY_OK` / `SILENT_BREAKAWAY_OK`: children that
   ask to break away get error 5. Tools that spawn long-lived helpers
   (`mspdbsrv`, Gradle daemon, `VBCSCompiler`, `sccache`) will keep a
   per-tree job alive after the root exits. Treat "root exited, members
   remain" as a normal state, not a leak.
5. **Crash Recovery and Job Re-Adoption / NFR-REL-2: the current design
   fails.** If the service crashes and nothing else holds a handle, the
   named job **disappears from the namespace while its members keep running
   under the cap**. The restarted service cannot find or lift it, which
   breaks "after a crash … re-adopts its named jobs" and fail-open. Options,
   in order of preference:
   1. Create jobs as **permanent objects** (`OBJ_PERMANENT`; SYSTEM has
      `SeCreatePermanentPrivilege`) and delete them explicitly with
      `NtMakeTemporaryObject` when the tree drains. **Not verified: needs a
      SYSTEM test.**
   2. Keep a duplicate of every job handle in a second long-lived process,
      for example a tiny `myclean-keeper` child of the SCM-restarted
      service, or the tray. Verified here: with a holder, reopen + reconcile
      + lift work.

   Pick one in G1 and add a scenario: "service killed, no other process
   restarted yet → the build is still reachable and its caps can be lifted."
6. **Name squatting:** any non-admin process can create
   `Global\MyClean.Job.build` first. The service's `CreateJobObject` would
   then return the attacker's object with `ERROR_ALREADY_EXISTS`, which the
   attacker can also control. Use a **private namespace**
   (`CreateBoundaryDescriptor` + `AddSIDToBoundaryDescriptor(SYSTEM)` +
   `CreatePrivateNamespace`), or at least treat `ERROR_ALREADY_EXISTS` on a
   fresh start as hostile unless the object's owner is SYSTEM
   (`GetSecurityInfo`). Re-adoption must apply the same owner check.
7. **Escalation Ladder step 4 (suspend) and Automatic Rollback:**
   - Suspends outlive the governor, so persisting each suspend before
     applying it, as the spec requires, is necessary.
   - Because `NtResumeProcess` is not owner-aware, orphan reconciliation
     must resume **only recorded (PID, creation time) pairs, exactly as many
     times as recorded**. Never "resume everything suspended", or MyClean
     will start processes that a debugger or a launcher deliberately
     created suspended.
   - Per-process suspend has a race: a child created between enumeration
     and suspend escapes.
   - For trees inside a MyClean job, the **job freeze** (class 18) is
     atomic and immune to stray resumes. But it is undocumented, does not
     catch processes added later, and also needs a reachable job handle to
     undo (implication 5). Treat it as an optional strategy behind a flag,
     with `NtSuspendProcess` as the default.
8. **Fail-Open on Clean Stop:** verified mechanics. Setting `ControlFlags = 0`,
   clearing `LimitFlags` and clearing notification limits on a live job lifts
   the caps immediately. Resumes work from any process.
9. **Game Mode (cap 75 % → 40 % and back):** changing `CpuRate` on a running
   job is safe and takes effect within one cycle (about 0.6 s).
10. **Packaging and testing (new risk):** Kaspersky quarantined a binary
    that statically imports `NtSuspendProcess` / `NtResumeProcess`, and
    heuristically blocked near-identical rebuilds. Product implications:
    - resolve `Nt*` through `GetProcAddress`;
    - **sign** the service and tray binaries;
    - run the signed build on this machine with Kaspersky active before the
      M3 exit;
    - document the AV exclusion as a fallback;
    - make the adapter-integration tests (architecture, `platform/*`
      dummy-workload jobs) report "blocked by AV" distinctly instead of
      failing obscurely.
11. **Not verified here:**
    - assignment with only `PROCESS_SET_QUOTA | PROCESS_TERMINATE` against
      an **elevated** target (needs the SYSTEM service);
    - `Global\` from session 0;
    - `OBJ_PERMANENT` jobs;
    - the 7500 cap on an idle machine;
    - Gradle (not installed).
