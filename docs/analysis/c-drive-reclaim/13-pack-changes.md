# 13 Knowledge pack changes (and the code they needed)

The owner's directive, relayed by the coordinator: MyClean cleans on its own what is 100 %
safe. Safe means pure cache, installer or update downloads, setup leftovers or temp that the
tool regenerates or never needs again. Owner data is never a clean case (docs/packs.md).
Every auto case below has `not_in_use` and, where a running install or download could be hit,
a `min_age_days` guard.

## New clean cases

| Case | Tier | Scope | Targets | Guards | GiB here | Why it is safe |
|---|:-:|:-:|---|---|---:|---|
| `wsl-docker/tmp-prune` (native `wsl tmp prune`) | auto | user | measures `…\Packages\*\LocalState\rootfs\tmp`; runs `find /tmp -mindepth 1 -maxdepth 1 -mtime +7 ! -name .*-unix -exec rm -rf {} +` inside each distribution as root | 7 days, inside Linux only, Docker Desktop's distributions skipped | ~93 | `/tmp` is temporary by definition. WSL 1 never empties it. Entries untouched for a week belong to no running program. |
| `visual-studio/package-payloads` | auto | system | `%ProgramData%\Microsoft\VisualStudio\Packages\*,version=*` | 30 days, not in use | 10.1 | Microsoft Learn: the cache is needed only to repair offline, and "the installer will download them when needed". `_Instances`, `_Channels` and `_bootstrapper` do not match. |
| `uzpainter/verification-pytest-temp` | auto | user | `%LOCALAPPDATA%\UzPainter\verification\*\pytest-temp` | 7 days, not in use | 9.5 | It is pytest's `--basetemp`, which pytest "clears blindly before each test run" (pytest docs). The run's logs and report beside it are not targets. |
| `gpu/nvidia-app-downloads` | auto | system | `%ProgramData%\NVIDIA Corporation\NVIDIA App\UpdateFramework\ota-artifacts\grd\*` | 14 days, not in use | 3.7 | Completed driver packages: 617.14 is the installed driver. The app's own pending update (`nvapp`) and its state are never targets. |
| `electron-apps/updater-downloads` | auto | user | `%LOCALAPPDATA%\*-updater\pending`, `…\installer.exe` | 14 days, not in use | ~3.5 | electron-updater's own sources: the cached installer is only the base of the next differential download, and a missing file is downloaded again. |
| `windows/powertoys-updates` | auto | user | `%LOCALAPPDATA%\Microsoft\PowerToys\Updates\*.exe` | 14 days, not in use | 1.5 | Update installers that are installed or superseded (six versions, 0.99 to 0.102). |
| `node/electron-download-cache` | auto | user | `%LOCALAPPDATA%\electron\Cache\*` | 30 days, not in use | 1.6 | Electron docs: the download cache; npm downloads a missing version again. |
| `node/bun-cache` | auto | user | `%USERPROFILE%\.bun\install\cache\*` | 30 days, not in use | 1.1 | Bun docs: the global cache (`bun pm cache rm` removes it); Bun downloads a missing package again. |
| `dev-tools/trivy-java-db` | auto | user | `%LOCALAPPDATA%\java-db\trivy-java.db`, `%LOCALAPPDATA%\trivy\java-db\trivy-java.db` | 7 days, not in use | 1.45 | Trivy fetches its databases automatically as needed. |
| `creative-apps/reallusion-downloads` | auto | user | `%LOCALAPPDATA%\Reallusion\RLDownloads\*.exe` | 30 days, not in use | 0.73 | Setups Reallusion Hub downloaded and ran. |
| `games/unreal-game-logs` | auto | user | `%LOCALAPPDATA%\*\Saved\Logs\*`, `…\Saved\Crashes\*` | 7 days, not in use | ~1.2 | The game's diagnostics, written again on every run. Config and SaveGames are never targets. |
| `games/unreal-game-shader-precache` | auto | user | `%LOCALAPPDATA%\*\Saved\*.ushaderprecache` | not in use | ~1.5 | Shader byte code the game rebuilds (category gpu-shader-caches, auto by the spec). |
| `vscode/webview-resource-cache` | **ask** | user | `%APPDATA%\Code\WebStorage\*\CacheStorage` | 2 days, not in use | 10.5 | VS Code's webview service worker caches local files (`vscode-resource-cache-N`). But a webview could keep its own caches in the same place, so it is not 100 % safe. |
| `creative-apps/autodesk-installers` | **ask** | system | `%SystemDrive%\Autodesk\*` | 30 days, not in use | 3.2 | Programs run without them, but Repair and some updates look for these files (Autodesk community answers). |
| `uzpainter/cache` | **ask** | user | `%LOCALAPPDATA%\UzPainter\cache\*\*` | 14 days, not in use | (1.3 later) | A cache, but what rebuilding it costs is the app's business. |

## Rules re-tiered from ask to auto (28)

| Rules | Why it is safe |
|---|---|
| `gpu/nvidia-dxcache`, `nvidia-glcache`, `d3dscache`, `amd-cache` | shader caches the driver rebuilds (one-time stutter); spec changed to "Shader caches are auto" |
| `windows/thumbnail-cache`, `internet-cache`, `delivery-optimization`, `temporary-setup-files`, `upgrade-discarded-files` (handlers) | caches and setup leftovers only; spec "Handlers of regenerable data are auto" |
| `node/pnpm-store`, `npm-cache`, `yarn-cache`, `python/pip-cache`, `uv-cache`, `dotnet/nuget-locals`, `go/cache` (native) | the tool's own prune; the tool downloads what it misses; spec updated |
| `zig/global-cache` | Zig rebuilds a missing entry (30 days) |
| `vcpkg/build-files`, `vcpkg/clone-build-files` (inactive 14 → 30 days, as auto artifact rules require) | vcpkg rebuilds build trees, staging and downloads; `--clean-after-build` removes them itself |
| `android/sdk-temp`, `android/user-cache` | interrupted SDK downloads; tools' download caches |
| `python-store/pip-cache` | pip's cache (30 days) |
| `rust/registry-cache` | Cargo restores crates, sources and checkouts (90 days) |
| `java-gradle/gradle-versions` | Gradle version caches and wrapper distributions unused for 90 days; downloaded again |
| `jetbrains/caches` | indexes the IDE rebuilds |
| `games/steam-depotcache` | manifests Steam fetches again (dropped by the Program Files deny-list on this machine) |
| `vscode/vsix-cache` | downloaded extension packages (30 days) |
| `windows/icon-cache` | Explorer rebuilds it; only offered while Explorer is not running |

**Kept ask, and why.** `playwright/browsers` (a project pinned to a removed revision fails until
`npx playwright install`), `vscode/workspace-storage` (extension state such as chat history),
`jetbrains/old-versions` (local history of old IDE versions), `vcpkg/binary-cache` (hours of
rebuilds), the Unity, Unreal and `.vs` artifacts of projects inactive 14 days, `wsl-docker/docker-prune`
(removes stopped containers; category changed to `containers`), `windows/live-kernel-reports`,
the handlers that remove crash evidence, rollback or the Recycle Bin, DISM, and the setup-log
handlers.

## New attribution areas (explanations for folders that had none)

- windows: `myclean-quarantine` (`?:\.myclean-quarantine`).
- wsl-docker: `distro-tmp` (now `clean-rule` → `tmp-prune`).
- gpu: `nvidia-app-downloads`, `ngx-models`.
- visual-studio: `packages` changed to `clean-rule`.
- vscode: `webview-resource-cache`, `cursor-data`.
- node: `electron-download-cache`, `bun-cache` replace `js-runtime-caches`.
- electron-apps: `updater-downloads`.
- uzpainter: `verification-pytest-temp`; `cache` changed to `clean-rule`.
- creative-apps: `reallusion-downloads`, `reallusion-data`, `reallusion-content`, `zbrush-data`,
  `zbrush-quicksave`, `marvelous-designer-assets`, `gaea-autosave`, `artengine`;
  `autodesk-installers` changed to `clean-rule`.
- adobe: `camera-raw`.
- games: `epic-launcher-data`, `unreal-game-logs`.
- ai-tools: `nexa-hyperlink`, `cortex-memory`, `claude-code-versions`.
- dev-tools: `trivy-java-db`.
- rust: `ort-binaries`.
- security-network: `kaspersky-data`.
- device-software: `corsair-icue`.
- windows `update-downloads`: the note now says a large folder is usually an update waiting for
  a restart.

Each changed pack's version was bumped (MINOR), its `sources` cite the documentation or "observed
on reference machine" facts above, and its fixtures have `match` and `no_match` with this
machine's real folder names.

## Relocation and compression

No new relocation recipe: no supported setting exists for the remaining large folders. The VS
package cache has a `CachePath` policy, but it is HKLM and applies only before an install, so
the payload case covers it instead. Existing recipes cover the WSL distribution, RUSTUP_HOME,
Gradle, Cargo and the Unreal DDC.

There are no new compression targets. Compression is now refused inside WSL distributions (03).

## Code changes (approved by the coordinator)

| Change | Files |
|---|---|
| native tool `wsl tmp prune`: lists distributions with `wsl -l -q` (UTF-16 decoded, names checked, Docker Desktop's skipped), measures `/tmp` with `du -sk` inside each, prunes with `find` through `wsl -d <name> -u root --exec` (no shell), runs every distribution even if one fails | `src/core/cleanup/native_command.{hpp,cpp}`, `tests/unit/cleanup/{fakes.hpp,native_command_test.cpp}` |
| `Exclusion::wsl_distribution`: the analyzer never counts, and CompressFolder never compresses, anything in a `LocalState\rootfs` or `Packages\<pkg>\rootfs` | `src/core/compress/compression.{hpp,cpp}`, `tests/unit/compress/*` |
| spec: shader caches auto, regenerable handlers auto, package-manager prunes auto (Docker's ask) | `.rulebook/tasks/phase2_free-c-drive/specs/cleaning/spec.md` |
| tests pinning the tiers | `tests/unit/clean/catalog_test.cpp` (`ShaderCachesAreAuto`, `PackageManagersAreNativeUserAuto`, handlers' auto set), `engine_test.cpp` (`ShaderCacheItemsAreAuto`) |
| docs | `docs/packs.md` (wsl-docker row) |

## Checks

- `myclean packs validate data\packs`: all 35 packs `ok`, together, with fixtures.
- `ctest -L packs|clean|cleanup|compress|rules|cleaning` in `build/c-analysis` (debug, `-j 6`,
  below-normal priority), after every data, tier and code change: **261/261 passed**.
- clang-format `--dry-run --Werror` on every changed C++ file: clean.
