## 1. Implementation
- [x] 1.1 X4 spike: job CPU hard cap and JOB_OBJECT_LIMIT_JOB_MEMORY on real MSVC/Cargo/Gradle builds, nesting under IDE jobs, NtSuspendProcess on trees; note in docs/spikes/X4-jobs.md; done in 4d7f3f4 (docs/spikes/X4-jobs.md)
- [x] 1.2 X5 spike: GameConfigStore, SHQueryUserNotificationState and PDH GPU counters on 3+ installed games including one on the USB SSD; note in docs/spikes/X5-games.md; done in a072b66 (docs/spikes/X5-games.md); spec revised with the findings in 951884a
- [x] 1.3 G1: process classes from data/classes.toml (system, game, allow, build, test, default; image + command-line + path + signer matching, first match wins); done in a3d23b7 (loader) and 442147e (classifier, data/packs/process-classes)
- [x] 1.4 G1: proactive per-tree jobs (permanent, SYSTEM-owned, nested under a class parent job or a foreign IDE job): build/test roots assigned on the ETW start event; CPU hard cap (default 75 %), commit cap (default 96 GB build / 64 GB test), notification limits at 80 %; completion-port tree tracking; done in 441171f (Win32 jobs) and 442147e/a8b8b69 (job book, wiring)
- [x] 1.5 G2: pressure detection (CPU > 90 % 15 s, available RAM < 8 GB, commit > 90 %, hard faults) with 60 s clear hysteresis; done in 442147e (pressure and clear hysteresis) and a8b8b69 (service wiring)
- [x] 1.6 G2: escalation ladder per heaviest non-protected tree with 10 s cooldown: priority + memory priority -> EcoQoS -> cap job -> suspend -> kill (opt-in per class, off by default); rollback of steps 1-4 when pressure clears; done in 442147e (ladder, cooldown, rollback; kill off by default)
- [x] 1.7 G2: fail-open: clean stop lifts caps and resumes suspends; restarted service re-adopts named jobs and resumes orphaned suspends; every step in governor_event + action_log; done in 442147e/a8b8b69 (governor/recovery: clean stop, re-adoption of named jobs, orphaned suspends resumed; governor_event and action_log)
- [ ] 1.8 G3: game detection (owner list, library roots, GameConfigStore, fullscreen + GPU) and game mode (protected game tree, tighter build cap default 40 %, stricter ladder) with tray override auto/on/off — detection and game mode done in 6663b35/442147e and governor.game_mode in the service; the tray override is still missing (with G4)
- [ ] 1.9 G4: tray notifications rate-limited to one per tree per 5 min with Resume / Kill / Allow once / Always allow; Pause governor for N minutes
- [ ] 1.9b Orphaned process trees: detect polling/watch loops (shell scripts, tail -f, sleep loops, inbox pollers) whose launching session is gone and that repeat the same command line many times; propose ending them, and end them automatically once the owner allows it per pattern (seen 2026-11-01: 88 orphaned watch_channel.sh / collab.py loops from closed Claude sessions)
- [ ] 1.10 Acceptance: stress scenario (parallel C++ + Rust builds + memory hog) keeps foreground probe latency < 100 ms with no commit exhaustion; a running game receives zero actions; killing the service then letting the SCM restart it leaves no orphaned cap or suspend; service overhead within NFR-PERF-1

## 2. Tail (docs + tests — check or waive with tailWaiver)
- [ ] 2.1 Update or create documentation covering the implementation
- [ ] 2.2 Write tests covering the new behavior
- [ ] 2.3 Run tests and confirm they pass
