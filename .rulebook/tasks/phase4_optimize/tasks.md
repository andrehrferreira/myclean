## 1. Implementation
- [ ] 1.1 X6 spike: StartupApproved value format and task/service disable-enable round trip; note in docs/spikes/X6-startup.md
- [ ] 1.3 O1: startup inventory (Run/RunOnce incl. WOW6432Node, Startup folders, StartupApproved state, logon/boot tasks via ITaskService, non-Microsoft auto services) with measured first-2-minutes impact; disable/enable via StartupApproved, task Enabled flag, service start type, with undo
- [ ] 1.5 O3: search-indexer scope: detect indexed build/cache folders and offer exclusion as an action with undo
- [ ] 1.6 O4: memory-leak detector on per-tree private bytes (sustained growth for hours) with trend alert
- [ ] 1.7 O5: boot and logon time tracking attributed to startup items and services
- [ ] 1.8 O6: background-app cost report (30-day CPU, RAM, disk, network per resident process)
- [ ] 1.9 O7: verify loop: before/after measurement for every applied optimization, alert or auto-undo when reversible and worse
- [ ] 1.11 S4: photo near-duplicates via WIC decode + 64-bit dHash, Hamming distance <= 6
- [ ] 1.14 P4: Dev Drive advisor (explain + guide; never creates the volume itself)
- [ ] 1.15 C6: WSL/Docker VHDX size report and compaction (ask tier; wsl --shutdown + compact)
- [ ] 1.16 I3: guided freeze elimination: pause one suspect at a time with approval, auto-restore, compare incident rates
- [ ] 1.17 Acceptance: prompt 2 passes; every applied optimization in the audit log has a verification result; NTFS index matches a full rescan on C: and F:

## 2. Tail (docs + tests — check or waive with tailWaiver)
- [ ] 2.1 Update or create documentation covering the implementation
- [ ] 2.2 Write tests covering the new behavior
- [ ] 2.3 Run tests and confirm they pass
