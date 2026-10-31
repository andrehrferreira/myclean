## 1. Implementation
- [x] 1.1 X1 spike: NVMe health log + temperature via IOCTL_STORAGE_QUERY_PROPERTY on all 5 internal NVMe drives, ATA SMART on the WD Reds, what the ROG USB bridge allows; note in docs/spikes/X1-disk-health.md
- [ ] 1.2 X2 spike: WH_KEYBOARD_LL timeout/removal behavior on 26200 and raw-input timing; note in docs/spikes/X2-input.md
- [x] 1.3 R1: myclean-svc service host (LocalSystem, restart on failure) honoring deploy contracts C3, C4, C7
- [x] 1.4 R2: named pipe \\.\pipe\myclean, length-prefixed JSON-RPC 2.0, DACL SYSTEM + owner SID, typed methods only (no raw paths/commands)
- [x] 1.5 T1: process collector via NtQuerySystemInformation(SystemProcessInformation), 1 s, grouped into trees with PID-reuse-safe parent links
- [x] 1.6 T3: CPU (per logical processor, % of max frequency, DPC/interrupt, queue) and memory (available, commit, standby/modified, compressed, hard faults/s) collectors
- [x] 1.7 T4: physical-disk (bytes/s, IOPS, latency, queue, % idle) and volume (size, free, trend) collectors with volume-to-disk mapping
- [x] 1.8 T5: GPU collector (engine utilization, dedicated/shared memory, per process)
- [x] 1.9 T2: ETW Microsoft-Windows-Kernel-Process and Kernel-Network sessions via krabsetw; persist proc_start
- [x] 1.10 T6: telemetry pipeline: per-collector lock-free ring, downsampler, history query API
- [x] 1.11 R3: myclean-tray shell: tray icon with state color + tooltip, toasts, single instance, quits on Local\MyClean.Tray.Quit (deploy contract C5)
- [x] 1.12 T7: alert engine: thresholds + change detection, rate limiting, snooze, tray badge
- [x] 1.13 H1: disk-health reader (NVMe health log + temperature, ATA SMART, USB fallback to reliability counters) every 10 min + daily snapshot
- [x] 1.14 H2: health history, delta alerts (any new media error / reallocated or pending sector), temperature thresholds, TBW endurance projection
- [x] 1.15 I1: input watchdog in the tray: raw input + clipboard listener + SendMessageTimeout probes; incidents > 250 ms stored with owner, open-clipboard holder, viewer chain, foreground app, hung windows, suspects
- [x] 1.16 I2: freeze view + mechanism classifier (delayed rendering, clipboard held open, viewer chain, slow keyboard hook, heavy payload)
- [x] 1.17 K1: trust signal (WinVerifyTrust incl. catalog, signer, user-writable path, KB match) + knowledge base v1 data
- [x] 1.18 R5: myclean CLI with --json for every read feature, including `status --json` (deploy contract C6) and `health`
- [ ] 1.19 R6: `myclean mcp` stdio server with read-only tools (health_snapshot, explain_process, storage/volume/disk queries)
- [ ] 1.20 R4: dashboard: live charts for CPU, memory and each disk, per-drive health cards, volume bars with days-until-full, top process trees
- [ ] 1.21 Acceptance: prompt 1 passes in Claude Code via myclean mcp; all 8 drives show health or an explicit 'not readable over USB'; service + tray within NFR-PERF-1 for 24 h; deployed with tools/deploy/deploy.ps1

## 2. Tail (docs + tests — check or waive with tailWaiver)
- [ ] 2.1 Update or create documentation covering the implementation
- [ ] 2.2 Write tests covering the new behavior
- [ ] 2.3 Run tests and confirm they pass
