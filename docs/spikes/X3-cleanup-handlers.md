# X3 — Windows cleanup handlers headless (spike note)

**Status:** verified non-elevated. The elevated and LocalSystem runs are still open (§6).
Run on 2026-10-31 on Windows 11 Pro 10.0.26200, not admin.
**Unblocks:** C3 (and the handler rules of C1). **Requirements touched:** cleaning spec,
"Windows System Areas Through Windows Handlers" (both scenarios) and "Native Prune Commands
Under the User Token"; F-011, F-013.

Nothing was cleaned. Each handler got `Initialize`/`InitializeEx` and `GetSpaceUsed` only.
`Purge`, `Deactivate` and `ShowProperties` were never called on a real handler, and no
registry value was written. DISM ran only `/AnalyzeComponentStore`, which is read only and was
refused anyway (exit code 740). The tools ran only their read-only "where is your cache"
queries.

## 1. Questions

- **(a)** Which handlers under `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\
  VolumeCaches` load and answer `Initialize` + `GetSpaceUsed` with no UI, and what sizes do
  they report for C:?
- **(b)** Which need elevation, and which want UI?
- **(c)** What has to run in the service (LocalSystem), and what in the tray (owner token)?
- **(d)** How can a handler that shows a window be detected and refused, without the owner
  ever seeing the window?

## 2. Method

The probe is `tests/unit/platform_cleanup/handler_probe_test.cpp`
(`HandlerProbe.ScansEveryRegisteredHandlerReadOnly`). It runs every registered handler through
`platform::Win32HandlerHost::probe` and prints one line per handler. The host does this for
each call:

1. Starts a fresh thread and assigns it to a **private desktop** created by the process
   (`CreateDesktopW`, `SetThreadDesktop`). A window that a handler opens there is never
   shown on the owner's screen and gets no input.
2. Initializes a single-threaded apartment on that thread. The handlers are registered
   `ThreadingModel=Both`, so they are created on this thread, on the private desktop.
3. Reads the handler's CLSID (the default value of its key, 64-bit view), creates it in process
   and calls `IEmptyVolumeCache2::InitializeEx` (or `Initialize` when the handler does not
   implement it). The handler's own key is passed in, opened `KEY_READ`.
4. Calls `GetSpaceUsed` with a progress callback that returns `E_ABORT` once the call is
   aborted.
5. Meanwhile the calling thread checks the private desktop every 50 ms for a visible window
   (`EnumDesktopWindows` + `IsWindowVisible`), and checks the timeout and the stop token. If any
   of these trips, the call is aborted through the callback. A handler that ignores the abort
   is abandoned after a grace period: its thread is left running.

**Input flags.** `Initialize` receives **no input flag**. The brief suggested
`EVCF_SETTINGSMODE`, but Microsoft documents that in that mode "cleanup must be handled by
IEmptyVolumeCache::Initialize" and "the handler should ... clean up any unneeded files
regardless of what drive they are on". In other words, `EVCF_SETTINGSMODE` is the
`cleanmgr /sagerun` *delete* mode. Passing it would have cleaned this PC during the scan. It is
never passed, and the fake-handler tests assert that the flags received are 0.
`EVCF_DONTSHOWIFZERO` is an *output* flag: the handler sets it, and MyClean leaves out
zero-byte handlers anyway.

## 3. Results (non-elevated, volume `C:\`)

The whole run took 2.9 s. No handler showed a window, timed out or was abandoned.
`init` is the HRESULT of `Initialize(Ex)`; `S_FALSE` means "nothing to delete", and then
`GetSpaceUsed` is not called. Flags: 0x001 `HASSETTINGS`, 0x002 `ENABLEBYDEFAULT`,
0x008 `ENABLEBYDEFAULT_AUTO`, 0x010 `DONTSHOWIFZERO`.

| Handler | DLL (CLSID) | Ex | init | flags | Reported | ms |
|---|---|---|---|---|---|---|
| Active Setup Temp Folders | DATACLEN (data-driven {C0E13E61}) | no | S_OK | 0x010 | 0 | 12 |
| BranchCache | {DE661907} | yes | **E_ACCESSDENIED** | — | — | 9 |
| Content Indexer Cleaner | DATACLEN {A9B48EAC} | no | S_FALSE | 0 | — | 6 |
| D3D Shader Cache | D3DSCache {D8D133CD} | yes | S_OK | 0 | 2.6 MiB | 918 |
| Delivery Optimization Files | {4057C1AD} | no | S_OK | 0 | **1.36 GiB** | 20 |
| Device Driver Packages | {DEF03231} | yes | S_FALSE | 0 | — | 13 |
| Diagnostic Data Viewer database files | data-driven | no | S_FALSE | 0 | — | 11 |
| Downloaded Program Files | occache {8369AB20} | no | S_OK | 0x00B | 0 | 13 |
| DownloadsFolder | data-driven | no | S_OK | 0 | **8.80 GiB** | 7 |
| Feedback Hub Archive log files | data-driven | no | S_FALSE | 0 | — | 11 |
| Internet Cache Files | ieframe {9B0EFD60} | yes | S_OK | 0x00B | 685 MiB | 37 |
| Language Pack | {191D5A6B} | yes | S_FALSE | 0 | — | 6 |
| Offline Pages Files | ieframe {8E6E6079} | yes | S_FALSE | 0x001 | — | 23 |
| Old ChkDsk Files | data-driven | no | S_OK | 0x010 | 0 | 4 |
| Previous Installations | setupcln {84e04a55} | no | S_FALSE | 0x010 | — | 15 |
| Recycle Bin | shell32 {5ef4af3a} | yes | S_OK | 0x001 | 19 MiB | 36 |
| RetailDemo Offline Content | data-driven | no | S_OK | 0x010 | 0 | 4 |
| Setup Log Files | data-driven | no | S_OK | 0x010 | 0 | 2 |
| System error memory dump files | data-driven | no | S_FALSE | 0 | — | 1 |
| System error minidump files | data-driven | no | S_FALSE | 0 | — | 1 |
| Temporary Files | DATACLEN {0B2C35D2} | no | S_OK | 0 | 0 | 1577 |
| Temporary Setup Files | setupcln | no | S_FALSE | 0x010 | — | 4 |
| Thumbnail Cache | thumbcache {889900c3} | yes | S_OK | 0x002 | 268 MiB | 16 |
| Update Cleanup | {606B3777} | no | S_FALSE | 0x010 | — | 8 |
| Upgrade Discarded Files | setupcln | no | S_FALSE | 0x010 | — | 4 |
| User file versions | {0F563B5F} | yes | S_OK | 0x010 | 0 | 25 |
| Windows Defender | data-driven | no | S_OK | 0x010 | 0 | 2 |
| Windows ESD installation files | setupcln | no | S_FALSE | 0x010 | — | 3 |
| Windows Error Reporting Files | data-driven | no | S_OK | 0x010 | 6.0 MiB | 85 |
| Windows Reset Log Files | setupcln | no | S_FALSE | 0x010 | — | 3 |
| Windows Upgrade Log Files | setupcln | no | S_FALSE | 0x010 | — | 2 |

The 32-bit registry view lists only 21 of these 31 keys, and some of its CLSIDs map to
`SysWOW64` DLLs. MyClean is 64-bit and reads the 64-bit view (`KEY_WOW64_64KEY`).

## 4. Findings

- **(a) Headless.** All 31 handlers load in process and answer with no window, even with
  `HASSETTINGS` set. That flag only adds a "Settings" button in cleanmgr, which calls
  `ShowProperties`, and MyClean never calls it. No handler has to be excluded for UI so far.
  The private-desktop check stays in place: a handler that opens a window in the elevated or
  SYSTEM run, or after a Windows update, is refused (`Errc::refused`). It is never shown, and
  it is never purged, because the window check runs before `Purge`.
- **(b) Elevation.** `BranchCache` refuses `Initialize` with `E_ACCESSDENIED`. The
  "Clean up system files" set returns `S_FALSE` when not elevated: Update Cleanup, Previous
  Installations, Temporary Setup Files, Upgrade Discarded Files, Windows ESD installation
  files, Windows Reset/Upgrade Log Files, Device Driver Packages, Language Pack and the two
  memory-dump handlers. Without elevation `S_FALSE` cannot tell "nothing to delete" from
  "cannot see it". These sizes come only from the elevated run (§6). `Delivery Optimization
  Files` reports 1.36 GiB without elevation (the DO service answers); its purge is expected to
  need admin.
- **(c) Which process.** Several handlers read **the caller's own profile**:
  Thumbnail Cache (`%LOCALAPPDATA%\Microsoft\Windows\Explorer`, 268 MiB), Internet Cache
  Files (685 MiB), D3D Shader Cache, Temporary Files (the user's `%TEMP%` part) and
  DownloadsFolder. Run as LocalSystem they would see `systemprofile`, not the owner's data
  (expected from where they look; to be confirmed in the SYSTEM run). So a handler is not
  always a service action. `HandlerCleanup` takes the rule's `scope`: `system` handlers run in
  the service, `user` handlers run in the tray under the owner's token, on the same host code.
- **Never offer as cleaning:** `DownloadsFolder` deletes the owner's Downloads folder
  (8.80 GiB of personal files, and the spec's old-downloads analysis covers it as `report`).
  `User file versions` deletes File History versions. `Recycle Bin` empties the owner's bin,
  so `ask` at most. `Previous Installations` and `Windows ESD installation files` remove the
  ability to roll back or reset Windows, so `ask`.
- **"Windows Update Cleanup"** (the spec's example) is the key `Update Cleanup`. It is
  registered on this build, but its 7.2 GB-style figure comes only from the elevated run.

### Suggested handler rules (for the catalog, C1)

| `scope` | Handlers |
|---|---|
| `system` (service) | Update Cleanup, Delivery Optimization Files, Windows Error Reporting Files, Temporary Setup Files, Setup Log Files, Windows Upgrade Log Files, Windows Reset Log Files, Upgrade Discarded Files, Old ChkDsk Files, System error memory dump files, System error minidump files, Device Driver Packages, Windows Defender, BranchCache, RetailDemo Offline Content, Diagnostic Data Viewer database files, Feedback Hub Archive log files, Content Indexer Cleaner, Language Pack (`ask`), Previous Installations (`ask`), Windows ESD installation files (`ask`) |
| `user` (tray) | Temporary Files, Thumbnail Cache, Internet Cache Files, D3D Shader Cache (`ask`, like the other shader caches), Downloaded Program Files, Offline Pages Files, Active Setup Temp Folders, Recycle Bin (`ask`) |
| not offered | DownloadsFolder, User file versions |

`handler` in a rule is the key name exactly as listed (it is not the display name).

## 5. Native prune commands and DISM

**Allow-list.** A rule with strategy `native` may name only one of the eight F-013 commands.
`cleanup::find_native_tool` rejects anything else; the catalog loader can use it at load time.
Each command's read-only query, run here through `Win32CommandRunner`, reported:

| Command (rule) | Runs | Cache query | This PC |
|---|---|---|---|
| `pnpm store prune` | same | `pnpm store path` | `F:\.pnpm-store\v11` (pnpm.cmd) |
| `npm cache verify` | same | `npm config get cache` | `C:\Users\…\AppData\Local\npm-cache` (npm.cmd) |
| `yarn cache clean` | same | `yarn cache dir` | `C:\Users\…\AppData\Local\Yarn\Cache\v6` (yarn.cmd) |
| `pip cache purge` | same | `pip cache dir` | `c:\users\…\appdata\local\pip\cache` |
| `uv cache prune` | same | `uv cache dir` | `C:\Users\…\AppData\Local\uv\cache` |
| `dotnet nuget locals all --clear` | same | `dotnet nuget locals all --list` | four labeled folders |
| `go clean -cache -modcache` | same | `go env GOCACHE GOMODCACHE` | `…\go-build`, `…\go\pkg\mod` |
| `docker system prune` | `docker system prune --force` | `docker system df --format {{.Type}}={{.Reclaimable}}` | Images 34.24 GB, Containers 232 MB, Build Cache 21.45 GB reclaimable; volumes are left out (`prune` keeps them without `--volumes`) |

`docker system prune` asks "Are you sure?" unless it gets `--force`. Standard input is `NUL`,
so without the flag it would read end of file and stop. npm, pnpm and yarn are `.cmd` shims, so
the runner starts them through `cmd.exe /d /s /c` and refuses any argument that contains a
cmd.exe metacharacter.

**User token.** `NativeCommand` refuses preview and apply with "user context is unavailable"
when its runner runs as LocalSystem, LocalService or NetworkService. So the service can
never run one. The spec scenario "Never as SYSTEM" is the refusal of a service-built action.

**DISM.** Without elevation, `/AnalyzeComponentStore` exits with **740**
(`ERROR_ELEVATION_REQUIRED`). `tests/unit/cleanup/fixtures/analyze-not-elevated.txt` is
that recording. MyClean reads only exit codes (`dism_status`) and English output
(`/English`), never localized text. The preview size is Microsoft's "overhead" (backups and
disabled features plus cache and temporary data), which is an upper bound, shown only when
DISM recommends a cleanup. `/ResetBase` is never run.

## 6. Open: needs the owner, elevated

1. From an **elevated** prompt, run the probe and keep its table:
   `build\release\tests\unit\platform_cleanup\myclean_platform_cleanup_tests.exe --gtest_filter=HandlerProbe.*`
   This answers the sizes of the `S_FALSE` set and whether any handler opens a window when
   elevated.
2. The same probe as **LocalSystem** (from the service, or `psexec -s` if it is installed)
   confirms (c): the per-user handlers should report ~0 there.
3. `dism /Online /Cleanup-Image /AnalyzeComponentStore /English` elevated. Save the output as
   a real fixture next to `analyze-docs-example.txt`. The success fixtures in use now are
   Microsoft's documented example and a "recommended" variant of it.

Even elevated, never run `/StartComponentCleanup`, `/ResetBase`, a `Purge` or `cleanmgr
/sagerun` as part of verification.
