# Proposal: phase5_guard

## Why
The owner wants to know whether anything suspicious runs on the machine, which apps are unused and safe to remove, and whether backups exist and are current, without MyClean ever removing anything on its own. Prompts 4, 5 and 6. Milestone M5 in docs/roadmap.md; evidence F-036..F-038.

## What Changes
Persistence-point and browser-extension scan with install origin; background network talkers over time; verdicts only from Defender and an opt-in hash-only VirusTotal lookup; installed apps (Win32 + Appx) with multi-source last-used and keep/remove verdict; uninstall through each app's own uninstaller after approval; backup detection (Windows, third-party, K:/L: drives) with last-backup age and repos with unpushed or uncommitted work. Work packages E1–E3, A1, A2, B1 and spike X8 in docs/dag.md.

## Impact
- Affected specs: security, apps, backup
- Affected code: src/core/{analyze,rules}, src/platform/{reg,wmi,appx,defender,tasks,services}, src/cli, data/kb
- Breaking change: NO
- User benefit: Security, app and backup visibility with evidence-based verdicts; prompts 4, 5, 6 answerable
