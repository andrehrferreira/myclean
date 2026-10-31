# Developer deployment (no installer)

`tools/deploy/deploy.ps1` installs and updates MyClean on this PC straight
from the source tree. It builds and tests every change. When the result is
green **and** different from what is installed, it swaps the service, the
tray app and the CLI to the new build, health-checks them, and rolls back on
failure.

## Usage

```powershell
pwsh -File tools\deploy\deploy.ps1              # build + test; deploy only if green and changed
pwsh -File tools\deploy\deploy.ps1 -Watch       # stay running; redeploy after every good change
pwsh -File tools\deploy\deploy.ps1 -Force       # redeploy even if the binaries are identical
pwsh -File tools\deploy\deploy.ps1 -Status      # installed version, service, tray, versions, history
pwsh -File tools\deploy\deploy.ps1 -Rollback    # go back to the previous installed version
pwsh -File tools\deploy\deploy.ps1 -Uninstall   # remove service + tray autostart; keep versions
pwsh -File tools\deploy\deploy.ps1 -Uninstall -Purge   # also delete all versions and %ProgramData%\MyClean (asks to type PURGE)
```

Other options: `-Preset <name>` (default `release`), `-KeepVersions <n>`
(default 5), `-HealthSeconds <n>` (default 5), `-QuietSeconds <n>` (default
10; how long watch mode waits after the last edit).

Run it from a **normal (non-elevated)** terminal. Each deploy shows **one UAC
prompt**, used for the service and Program Files steps only. The tray is
started from the non-elevated half so it runs with your own token. From an
elevated terminal the deploy still works, but the tray is left for the next
logon.

## What one deploy does

```
 non-elevated                                  elevated (UAC)
 ─────────────────────────────────────────     ─────────────────────────────────────────────
 1 enter VS dev shell if cl.exe is missing
 2 cmake --preset → build → ctest   (gate)
 3 cmake --install → out\stage
 4 hash staged files; same as installed?
   → "up to date", stop here
 5 write manifest.json (version, hash, git)
 6 stop tray (quit event, then kill after 10 s)
 7 ───────────────────────────────────────────▶ 8  copy stage → versions\<version>
                                                9  stop service (SCM stop, kill after 30 s)
                                               10  point current → versions\<version>
                                               11  register / fix service if needed
                                               12  start service; must stay up, same PID, 5 s
                                                   └ fails → current → previous, restart, report
                                               13  prune old versions (keep 5, never current/previous)
 14 ◀──────────────────────────────────────────
 15 HKCU Run autostart → current\bin\myclean-tray.exe
 16 start tray; must stay alive 5 s
 17 `myclean status --json` must exit 0
    └ 16 or 17 fail → elevated rollback to previous
```

"Good change" means: configure, build (`/W4 /WX`) and the full `ctest` suite
all succeeded. Lint (clang-tidy) runs in the commit hook and CI, not here.

In `-Watch` mode the script watches `src/`, `data/`, `tests/`, `cmake/` and
the root `CMakeLists.txt`, `CMakePresets.json` and `vcpkg.json`. After the
last edit it waits `-QuietSeconds`, then runs the same pipeline. A red build
or red tests print the error and the installed version keeps running. Edits
to `docs/`, `build/`, `out/` and `.rulebook/` are ignored.

## On-disk layout

```
C:\Program Files\MyClean\
  versions\
    20261101-142310-3fa9c2d1\        one folder per deployed build (timestamp + content hash)
      bin\  myclean-svc.exe  myclean-tray.exe  myclean.exe  *.dll  *.pdb
      data\ rules\  classes.toml  kb\
      manifest.json                  version, content hash, git commit/dirty, preset, components
    …                                up to 5 kept
  current  ──junction──▶ versions\<active>
C:\ProgramData\MyClean\deploy\
  deploy.log                         every privileged step
  history.jsonl                      one line per activation: version, active | failed
```

- The service is registered once, as `"C:\Program Files\MyClean\current\bin\myclean-svc.exe"`.
  An update only moves the `current` junction, so the service registration
  never changes.
- New versions go to a new folder, so files still held open by an old
  process (for example a `myclean mcp` started by Claude Code) never block an
  update. Restart that client to pick up the new version.
- Binaries live under Program Files on purpose: the service runs as SYSTEM,
  and its executable must not be writable by a normal user.
- The script works in the 32-bit `pwsh` installed today. A 64-bit PowerShell
  7.4+ is still recommended, because 7.2 is out of support.

## Contract the code must honor

The script depends on these behaviors. They are part of the work packages
named in parentheses (see [dag.md](dag.md)).

| # | Contract | WP |
|---|---|---|
| C1 | `CMakePresets.json` has configure, build **and** test presets named `release`, with `binaryDir` = `${sourceDir}/build/${presetName}` (or another path without other macros) | F1 |
| C2 | `install()` rules place executables and DLLs (+ PDBs) in `bin/` and data files in `data/`; `cmake --install` produces a complete, relocatable tree | F1 |
| C3 | The service is named `MyClean`. On SCM stop it lifts every governor cap, resumes anything it suspended, flushes the DB and reports `SERVICE_STOPPED` within 30 s | R1, G2 |
| C4 | The service finds its data relative to its own executable (`..\data`) and its state under `%ProgramData%\MyClean`. It never writes into its version folder | R1 |
| C5 | The tray is single-instance per session and exits cleanly when the named event `Local\MyClean.Tray.Quit` is signaled | R3 |
| C6 | `myclean.exe status --json` exits 0 when the service is reachable and healthy, and non-zero with a JSON reason otherwise. It answers within 20 s | R5 |
| C7 | A crash during the first 5 s after start counts as a failed deploy, so startup errors must crash or stop the service, not leave it half-running | R1 |

Until a binary exists the script skips it. In M0 there is no executable
yet, so the script stops with a clear message after the build and tests.

## Recovery

- `-Status` shows what is active and the last 10 activations.
- `-Rollback` activates the newest version older than the current one.
- If even the rollback fails, `deploy.log` has every step. The service can
  be fixed by hand with `sc.exe` and the `current` junction pointed at any
  folder under `versions\`.
