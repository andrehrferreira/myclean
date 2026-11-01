# 15 Follow-ups for code (not done here)

1. **`wsl tmp prune` timeouts.** With 39,000 folders and 94 GiB, `du -sk /tmp` under WSL 1 may
   exceed the generic 2-minute locate timeout, and the deletion may exceed the 30-minute prune
   timeout. The tool needs its own longer timeouts. Measuring could also use `du` with a depth
   limit.
2. **Wording for native plans.** The executor's `spec.undo` ("the tool downloads what it needs
   again") and `NativeCommand::describe()` fit package caches. They read wrong for the `/tmp`
   prune. (src/core/cleaning is another agent's.)
3. **Imported WSL distributions** (`wsl --import` into `<dir>\rootfs`) match neither the
   `tmp-prune` targets nor the compression exclusion. Reading BasePath from the Lxss keys would
   cover them, as the relocation recipe already does.
4. **Placement target selection.** `scan-all` said "no healthy drive keeps 10 % free" for 11 GiB.
   But D:, W:, K: and L: have 0.99–5.9 TB free. Probably a bug in the volume filter (01).
5. **Compression estimate on `Packages`.** It is fixed by `Exclusion::wsl_distribution` (03).
   `scan-all` should show the new estimate after a rebuild.
6. **SoftwareDistribution.** Deleting it stays forbidden (the %WINDIR% deny-list, and a staged
   update). The useful signal is "update staged, no restart for N days". `scan-all` could raise
   that as an alert.
7. **Squirrel apps' old versions** (Discord three `app-*` folders, MongoDB Compass two): only the
   newest is used. Selecting "all but the newest" needs an engine feature. It is ~1.2 GiB here.
8. **Native commands not allowed yet:** `dart pub cache clean` (0.8), `bun pm cache rm` (the Bun
   case deletes entries instead), `npm cache clean --force` inside WSL.
9. **vcpkg downloads in `%LOCALAPPDATA%\vcpkg\downloads`** (1.7, tools such as perl) are cleaned
   only when VCPKG_DOWNLOADS points there. The vcpkg pack could add that location after checking
   which vcpkg versions default to it.
10. **NVIDIA app running.** Clean cases cannot require that a program is not running, only that
    nothing is open inside the target. A `require = { not_running = [...] }` would let the NVIDIA,
    VS Installer and electron-updater cases say so explicitly.
11. **The commit charge** of ~206 GB on 128 GB of RAM explains the page file size. Which process
    commits it is for the governor (02).
12. **Repository state seen during this task:** a sub-agent reported `core.bare = true` at one
    point. It read `false` later. Worth checking that nothing flips it.
