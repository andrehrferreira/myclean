# 3. Automatic system-space changes when C: is critically low (PLC-2)

**Status**: accepted
**Date**: 2026-11-01

## Context

PRD PLC-2 says the page file, hibernation and restore-point storage are "changed only on approval". On 2026-11-01 the reference machine's C: was nearly full while the owner worked. Asking first left the disk full until the owner had time for the tray. The owner then ordered that MyClean act on its own when C: is critically low. Commit bc98959 (2026-11-03) implemented the order, and the P0 audit (docs/analysis/p0-audit/02-gaps.md, PLC-2) flagged the code as an undocumented deviation.

## Decision

These settings are changed only on approval, except while the system volume has less than 10 % free. Then every scheduled scan runs `Executor::reclaim_system_space` before the approved plans, which submits automatic plans (actor scheduler):

- full hibernation becomes reduced;
- a system-managed or uncapped page file on C: becomes 4096 MB on C: (kept for crash dumps), plus a system-managed page file on the best healthy volume, so the total page-file capacity never shrinks;
- what a relocation recipe marked `automatic` names on C: moves to the best healthy volume, only while none of the recipe's `needs` images runs (today the WSL 2 disks and rustup home).

Shadow storage (restore points) is never changed automatically. Each group is tried at most once in 7 days, so a change that did not take effect is not repeated every hour. Every change is undoable from the tray, and every step is in `action_log`.

## Alternatives Considered

- Approval only, as the PRD said: rejected by the owner. A full C: costs more than these reversible changes.
- Also capping shadow storage: rejected, because deleting restore points cannot be undone.

## Consequences

- PRD PLC-2 now states the exception and points here.
- Shadow storage stays approval-only.
- A console run (`--console`) never applies system settings.
