# 07 ~\AppData\Local (276.4 GiB)

`Packages` (136.1) is covered in 03 (the WSL rootfs, 125.2) and below. "Kind" is one of: cache,
leftover, installer, app data, owner data. The pack cases named are those after this task (13).

| GiB | Folder | Date | What | Kind | Reclaim | Status |
|---:|---|---|---|---|---|:-:|
| 125.2 | `Packages\CanonicalGroupLimited.Ubuntu24.04LTS_…\LocalState\rootfs` | | WSL 1 distribution | — | 03 | 1 / 2 |
| 6.9 | `Packages\PythonSoftwareFoundation.Python.3.12_…` | 2026-04 | Store Python. `LocalCache\local-packages` 6.0 is `pip --user` packages (the owner's); pip's cache is inside it | app data | `python-store/pip-cache` (now auto, 30 days); user packages: `pip uninstall` | 1 / 3 |
| 2.5 | `Packages\MSTeams_8wekyb3d8bbwe` | | new Teams (WebView2) | cache + app data | `electron-apps/teams` (auto) for its caches | 1 |
| 0.9 | `Packages\OpenAI.Codex_…\LocalCache` | | Codex app | app data | — | – |
| 46.7 | `UzPainter` | 2026-10 | `oracle-work` 30.9 (envelopes 12.7, stability-study 3.8, quarantine 1.4, runs), `verification\<run>\pytest-temp` 9.5, `oracle-inbox` 2.9, `parity-store` 1.9, `cache` 1.3, smaller stores | owner's work + test temp + cache | **new `uzpainter/verification-pytest-temp`** (auto, 7 days): it is pytest's `--basetemp`, which pytest itself clears blindly before each run. `uzpainter/cache` (new, ask). The work stores are the owner's | 1 / 3 |
| 21.9 | `Google\Chrome\User Data` | | Default profile 16.6 (Service Worker 7.6, WebStorage site 3426 1.7, caches), `OptGuideOnDeviceModel` 4.0 (Gemini Nano) | profile + site data + model | caches: `browsers/chrome-cache`, `chrome/dawn-caches` (auto). Site data and the model are the owner's: chrome://settings/content/all, policy `GenAILocalFoundationalModelSettings = 1` | 3 |
| 14.3 | `Temp` | | `swap.vhdx` 6.5 (the running WSL 2 / Docker VM), `qpgzmvzm` 3.8 (2026-10-29), `claude` 1.0, DockerDesktop 0.7 | temp | `windows/temp-user` (auto, older than 2 days, not in use) | 1 |
| 8.2 | `Android\Sdk` | 2022-08 | `system-images\android-36` 4.3, `ndk\28.2` 2.1, emulator 1.1 | toolchain | SDK Manager | 3 |
| 7.0 | `Microsoft` | | OneDrive 2.2 (ListSync 1.5: Microsoft Lists sync; version folder 0.5; logs 0.2), **PowerToys\Updates 1.8**, VisualStudio 1.3 (Roslyn cache 0.9) | app data + installers | **new `windows/powertoys-updates`** (auto, 14 days) | 1 |
| 3.3 | `ms-playwright-mcp` | 2026-10 | browser profiles of the Playwright MCP server | owner data (logins) | review | 3 |
| 2.8 | `Programs` | | Polygonflow Dash 1.9, REDlauncher 0.9 | programs | uninstall | 3 |
| 2.7 | `UnrealEngine` | 2026-10 | Zen storage 1.6, shared DDC 0.8 | cache | relocate the DDC (`unreal/ddc`) | 2 |
| 2.0 | `vcpkg` | 2024-08 | `downloads` 1.7 (tools: perl 0.85) | download cache | not covered: `%LOCALAPPDATA%\vcpkg\downloads` is used only when VCPKG_DOWNLOADS points there (15) | 3 |
| 2.0 | `NVIDIA` | | DXCache 1.8 | shader cache | `gpu/nvidia-dxcache` (now auto) | 1 |
| 1.6 | `Discord` | 2026-09 | `app-1.0.9258`, `9259`, `9260` (0.44 each), packages | program | Squirrel keeps old `app-*` folders; only the newest is used (15) | 3 |
| 1.6 | `electron\Cache` | 2025-09 | Electron zips downloaded by npm | download cache | **new `node/electron-download-cache`** (auto, 30 days) | 1 |
| 1.55 | `npm-cache` | 2026-10 | `_npx` 1.1, `_cacache` 0.4 | cache | `node/npm-cache` (`npm cache verify`, now auto) | 1 |
| 1.45 | `java-db` | 2026-10 | `trivy-java.db` | downloaded DB | **new `dev-tools/trivy-java-db`** (auto, 7 days) | 1 |
| 1.45 | `ms-playwright` | | test browsers | download | `playwright/browsers` (ask: a project pinned to a removed revision fails until `npx playwright install`) | 3 |
| 1.24 | `ort.pyke.io\dfbin` | 2025-09 | ONNX Runtime binaries downloaded by Rust builds (four builds) | download cache | new area `rust/ort-binaries` (review) | 3 |
| 1.16 | `Reallusion` | 2025-05 | RLDownloads 0.73 (installers), ActorCore AccuRIG 0.35 | installers + app data | **new `creative-apps/reallusion-downloads`** (auto, 30 days) | 1 |
| ~3.9 | `*-updater` (12 folders: `@trezorsuite-desktop`, `accurig`, `actorcore-accurig`, `binance`, `hyperlink`, `ledger-live-desktop`, `lens`, `lm-studio`, `netmarblelauncher`, `noodl`, `productmanager`, `slobs-client`, `unityhub`) | 2021–2026 | electron-updater's `installer.exe` and `pending\<setup>.exe` | installer leftovers | **new `electron-apps/updater-downloads`** (auto, 14 days) | 1 |
| 0.99 | `camoufox` | 2025-09 | Camoufox (Firefox-based automation browser) cache with fonts | cache | — (under 1 GiB; no source) | 3 |
| 0.84 | `Adobe` | | Acrobat, Substance Painter | app data | review | – |
| 0.80 | `MongoDBCompass` | 2025-04 | `app-1.45.4` and `app-1.46.0` (Squirrel) | program | old `app-*` (15) | 3 |
| 0.79 | `Pub\Cache` | 2022-08 | Dart/Flutter packages | cache | `dart pub cache clean` (not an allowed native command, 15) | 3 |
| 0.79 | `pnpm` | | store 0.34, tools 0.35 | cache | `node/pnpm-store` (now auto) | 1 |
| 0.68 | `OpenAI\Codex` | 2026-10 | bin 0.40 (a copy of codex.exe), runtimes 0.27 | app | — | – |
| 0.67 | `Steam\htmlcache` | | Steam's web cache | cache | — (outside the Program Files case) | 3 |
| ~2.5 | `b1`, `ABInfinite`, `Wardogs`, `SCUM`, `TL`, `EpicGamesLauncher` … `\Saved` | 2024–2026 | Unreal games: shader byte code 1.5, logs 0.7, crash reports 0.5 | cache + logs | **new `games/unreal-game-shader-precache`, `games/unreal-game-logs`** (auto) | 1 |
| 0.50 | `Path of Exile 2\Shaders` | | game shader cache | cache | — | 3 |
| 0.50 | `UzEngine\logs` | 2026-03 | logs | logs | — | 3 |
| 0.50 | `Autodesk` | | Identity Services logs 0.29 | logs | — | – |
| 0.48 | `nvm` | 2026-10 | Node versions of nvm-windows | toolchains | `nvm uninstall <v>` | 3 |
| 0.39 | `CrashDumps` | 2026-10 | app crash dumps (Substance Painter) | dumps | `windows/crash-dumps` (auto, 7 days) | 1 |
| 0.35 | `enshrouded\caches` | 2024-07 | game cache | cache | — | 3 |
| 0.31 | `ConnectedDevicesPlatform` | | activity history | app data | — | – |
| 0.31 | `zig` | | global cache | cache | `zig/global-cache` (now auto) | 1 |
| 0.30 | `.dartServer` | | Dart analysis cache | cache | — | 3 |
| 0.25 | `JxBrowser\7.22`, `fanal` (Trivy's scan cache) | 2022 / 2024 | caches | cache | — | 3 |

Under 256 MiB: about 3.6 GiB in other folders.
