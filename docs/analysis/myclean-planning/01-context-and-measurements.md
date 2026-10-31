# 01 — Context and measurements

What the owner reported, and what a read-only sweep of the machine showed on
2026-10-31. Every later file builds on these findings.

## Reported problems

1. **C: is always short on space.** Temp files and unknown leftovers fill it;
   the owner cannot tell what is safe to discard.
2. **Developer clutter.** Many projects in many languages leave build outputs,
   caches and temp files behind.
3. **The machine feels slow.**
4. **Ctrl+C / Ctrl+V sometimes freezes the screen** (recent).
5. **Builds and test runs sometimes eat all CPU/RAM** and take the machine
   down. The owner wants a tray daemon that caps them. Games behave similarly
   and must not be throttled by mistake.
6. **Seven assistant-style prompts** (health check, startup apps, storage,
   unused apps, intruder scan, backups, monthly report) must become things
   MyClean can answer. See [06-assistant-prompts.md](06-assistant-prompts.md).

## Machine snapshot

| Item | Value |
|---|---|
| CPU | AMD Ryzen 9 7950X3D: 16 cores / 32 threads, 2 CCDs (core chiplets); only one has the 3D V-Cache |
| RAM | 127.2 GB (28.6 GB free at sample time) |
| Board | ASUS (Armoury Crate installed) |
| OS | Windows 11 Pro 10.0.26200 |
| Pagefile | system-managed |

| Volume | Label | Disk type | Size | Free |
|---|---|---|---|---|
| C: | SO | SSD | 952 GB | **84 GB (8.8 %)** |
| D: | SSD AR | SSD | 3.7 TB | 1.0 TB |
| E: | Tools & Games | SSD | 954 GB | 159 GB |
| F: | WS | SSD | 1.8 TB | 294 GB |
| G: | ToS | SSD | 1.8 TB | 334 GB |
| W: | Devkit & Assets | SSD | 3.7 TB | 1.76 TB |
| K: | Backup 1 | HDD | 9.3 TB | 5.1 TB |
| L: | Backup 2 | HDD | 9.3 TB | 5.9 TB |

All volumes are NTFS. There is no ReFS volume, so no Windows Dev Drive.

## Findings

### F-001 — C: is the only volume under pressure
- **Evidence:** `Get-Volume`: C: has 84 GB free of 952 GB. Every other volume
  has at least 159 GB free, and D: and W: have more than 1 TB each.
- **Impact:** C: does not need deeper deletion so much as relocation. Much of
  what lives on C: could move to a disk with free space.
- **Confidence:** high.

### F-002 — Hibernation and page files take ~112 GB of C:
- **Evidence:** `C:\hiberfil.sys` is 50.9 GB and `C:\pagefile.sys` is 60.9 GB.
  The page file is system-managed and sized from 127 GB of RAM.
- **Impact:** This is the largest single recoverable block. Possible fixes:
  `powercfg /h /type reduced` (keeps Fast Startup, drops full hibernate) or
  `/h off`; move the page file to D:, or cap it. All of these change system
  settings: they are reversible but need the owner's consent.
- **Confidence:** high for the sizes. The exact savings from the "reduced"
  hibernate type need measuring.

### F-003 — The user temp folder is 13.5 GB, and TEMP/TMP point to C:
- **Evidence:** `%TEMP%` = `C:\Users\<user>\AppData\Local\Temp` holds 13.5 GB.
  `TEMP` and `TMP` both point there.
- **Impact:** Compilers, installers and test runners write here, so it keeps
  growing back. Cleaning it fixes the symptom. Pointing the user TEMP/TMP
  variables at another SSD fixes the cause. Both belong in MyClean.
- **Confidence:** high.

### F-004 — Windows-managed caches hold ~21 GB
- **Evidence:** `C:\Windows\SoftwareDistribution\Download` 7.2 GB,
  `C:\Windows\Installer` 10.9 GB, `C:\ProgramData\Package Cache` 2.8 GB.
- **Impact:** Recoverable, but only through Windows' own cleanup handlers
  (see F-011). Deleting from `Windows\Installer` by hand breaks
  uninstall/repair of installed programs.
- **Confidence:** high for the sizes. How much is safely reclaimable needs
  measuring per handler.

### F-005 — Developer caches on C: total ~9 GB and keep growing
- **Evidence:** `.gradle` 4.2 GB, `.cargo` 2.9 GB, `npm-cache` 1.4 GB, `.nuget`
  0.3 GB, Unreal `%LOCALAPPDATA%\UnrealEngine` 2.6 GB. `Downloads` holds
  another 8.8 GB.
- **Impact:** Every package manager can move its cache with an environment
  variable or a config key (`GRADLE_USER_HOME`, `CARGO_HOME`, `npm config set
  cache`, `NUGET_PACKAGES`, `PIP_CACHE_DIR`, `GOMODCACHE`/`GOCACHE`, Unreal DDC
  path). Relocating them stops the regrowth.
- **Confidence:** high. Build outputs inside project folders on F:/W: have not
  been measured yet; a project scan comes in phase 1.

### F-006 — Two large HDDs are labelled as backup targets
- **Evidence:** K: "Backup 1" and L: "Backup 2", 9.3 TB each, 5+ TB free.
- **Impact:** Prompt 6 (backups) must look at what is on these drives, not only
  at Windows' backup settings. They also suit MyClean's quarantine overflow
  and the monthly report archive.
- **Confidence:** high that the drives exist; their contents were not
  inspected.

### F-007 — No Dev Drive exists
- **Evidence:** every volume is NTFS.
- **Impact:** On Windows 11 a Dev Drive (a ReFS volume with Defender's
  performance mode) cuts antivirus overhead on builds and package installs.
  Moving active repositories and package caches onto one targets both the
  slowness and the C: pressure. Creating one is a manual, owner-approved step
  that MyClean should only guide, not perform.
- **Confidence:** medium. The speed gain must be measured on this machine.

### F-008 — Recorded app hangs do not point at a clipboard culprit
- **Evidence:** Application log, event 1002, last 60 days: a Codex sandbox
  service message (8), Adobe Substance 3D Painter (4), SystemSettings (3),
  uzengine (3), userinit (2).
- **Impact:** Ctrl+C/V freezes are not being logged as app hangs. They need
  live measurement, which is the subject of
  [04-input-and-clipboard-freezes.md](04-input-and-clipboard-freezes.md).
- **Confidence:** high.

### F-009 — Several resident programs hook the keyboard or the clipboard
- **Evidence:** running: `PowerToys.KeyboardManagerEngine` (a low-level
  keyboard hook), `AnyDesk` (clipboard sync), Armoury Crate (4 processes),
  Discord (6), `copilot-runtime`, and 21 `msedgewebview2` processes. Clipboard
  history service `cbdhsvc` is running; the history registry values are not
  set, so the defaults apply.
- **Impact:** These are the first suspects for the Ctrl+C/V freeze (ranked in
  file 04). A no-code test is available today: quit Keyboard Manager and
  AnyDesk for a day and see whether the freeze goes away.
- **Confidence:** medium. They are suspects, not proven causes.

### F-010 — The Windows Search indexer is on
- **Evidence:** `WSearch` is Running/Automatic.
- **Impact:** If the indexer covers project folders, every `node_modules` or
  `target` rebuild triggers re-indexing. MyClean should check which folders
  are indexed and offer to exclude build-output folders.
- **Confidence:** medium. The indexed scopes were not read yet.
