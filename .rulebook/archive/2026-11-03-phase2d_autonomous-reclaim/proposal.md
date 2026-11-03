# Proposal: phase2d_autonomous-reclaim

## Why
The owner (C: at 5 % free) wants MyClean to free space on its own: every 100 %-safe cleanup runs without approval, reversible reductions happen automatically, and every other finding of the deep C: analysis becomes a one-click action.

## What Changes
Auto tier for regenerable caches, handlers and native prunes; hourly auto run and run after every scan; purge of quarantines on low-space volumes; cross-volume folder quarantine; apply progress in the tray; WSL /tmp prune from inside Linux; compression never on WSL rootfs; new auto cases from docs/analysis/c-drive-reclaim; automatic hibernation reduce and page file moved to a healthy NVMe (never shrunk below the commit in use) when C: is below 10 % free; WSL distro and rustup relocation when idle; scan-all reads disk health from the service so cold data gets archive targets; one-click tray actions for owner decisions (move to HDD, uninstall).

## Impact
- Affected specs: owner-decisions (one-click tray actions for owner decisions)
- Affected code: none
- Breaking change: NO
- User benefit: none
