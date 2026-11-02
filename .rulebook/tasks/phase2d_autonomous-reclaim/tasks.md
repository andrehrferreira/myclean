## 1. Implementation
- [x] 1.1 Auto runs hourly and after every scan; purge quarantine runs on volumes below 10 % free (clean-progress); done in ba1b656
- [x] 1.2 Apply progress in cleaning.status and the tray (clean-progress); done in ba1b656
- [x] 1.3 Cross-volume quarantine of folder items (clean-progress); done in ba1b656
- [x] 1.4 WSL /tmp prune native tool + pack rule, compression excludes WSL rootfs, auto tiers for caches/handlers/native prunes, new auto cases (c-analysis); done in 888621f
- [x] 1.5 scan-all takes disk health from the service; archive/relocate targets on healthy HDDs (scanall-health); done in bf535e7
- [ ] 1.6 Automatic hibernation reduce and page file moved (system-managed) to a healthy NVMe with room, small one kept on C: for dumps, when the system volume is below 10 % free; never shrink below the commit the machine uses (~202 GB on 128 GB RAM)
- [ ] 1.7 Automatic WSL distro move and RUSTUP_HOME relocation to a healthy drive when not in use
- [ ] 1.8 One-click tray actions for owner-decision findings: move to HDD archive, launch uninstaller
- [x] 1.9 DISM StartComponentCleanup timeout raised (timed out at 30 min under load); done: analyze and cleanup timeouts are 90 min (dism_cleanup.hpp)
- [ ] 1.10 Deploy and measure C: free space before/after

## 2. Tail (docs + tests — check or waive with tailWaiver)
- [ ] 2.1 Update or create documentation covering the implementation
- [ ] 2.2 Write tests covering the new behavior
- [ ] 2.3 Run tests and confirm they pass
