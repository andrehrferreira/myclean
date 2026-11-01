# 11 Browsers, chat apps, games and creative apps

## Browsers: cache vs profile

| GiB | Where | Cache (MyClean, auto) | Profile and site data (owner) |
|---:|---|---|---|
| 21.9 | Chrome `User Data` | `Default\Cache` 0.41, `Code Cache` 0.52, GPU and Dawn caches (`browsers/chrome-cache`, `chrome/dawn-caches`) | **Service Worker 7.6** (the offline caches of sites; the folder has been unchanged since 2022-10), IndexedDB 3.1, Extensions 2.6, WebStorage 1.9 (one site 1.7), the on-device model `OptGuideOnDeviceModel` 4.0 |
| 0.67 | Edge `User Data` | `browsers/edge-cache` | profile |
| 0.15 | Firefox `cache2` (Local) | `browsers/firefox-cache` | profiles in Roaming 0.12 |
| 3.3 | Playwright MCP profiles | — | logins of the MCP browser profiles |
| 0.99 | Camoufox cache | — (no source) | — |

Owner actions for Chrome: chrome://settings/content/all lists sites by storage, so clearing the
few biggest frees most of the 7.6 + 1.9. The policy `GenAILocalFoundationalModelSettings = 1`
removes the 4.0 GiB model and stops it coming back.

## Chat apps

| GiB | App | Cache (auto) | Rest |
|---:|---|---|---|
| 2.5 | new Teams (WebView2) | `electron-apps/teams` | profile |
| 1.6 + 0.36 | Discord | `electron-apps/discord` | three `app-*` versions 0.44 each (Squirrel; old ones, 15) |
| 0.65 | Slack (folder unchanged since 2022) | `electron-apps/slack` | Service Worker 0.61 (site data) |
| 0.76 | WhatsApp (folder unchanged since 2022) | — | Service Worker CacheStorage 0.75 (site data) |

## Games and launchers on C:

| GiB | What | Reclaim |
|---:|---|---|
| 4.4 | Steam client in Program Files (x86): `userdata` 2.75 (screenshots, cloud saves), client 1.7. The libraries are on other drives | `games/steam-httpcache` (auto), `steam-depotcache` (now auto); both are dropped by the Program Files deny-list here |
| 0.79 | Epic Online Services | — |
| 1.66 | Epic Games Launcher data in ProgramData | new area `games/epic-launcher-data` |
| 0.36 + 0.29 | Epic launcher `Saved` (web cache, logs) | `games/epic-webcache` (auto); logs and crashes via the new `unreal-game-logs` |
| ~2.5 | Unreal games' `Saved` (b1 0.59, ABInfinite 0.58, Wardogs 0.53, SCUM 0.49, TL 0.37): shader byte code 1.5, logs 0.7, crash reports 0.5 | **new `games/unreal-game-shader-precache`, `games/unreal-game-logs`** (auto) |
| 0.86 | REDlauncher (CD Projekt) | uninstall |
| 0.50 | Path of Exile 2 shader cache | — |
| 0.35 | Enshrouded caches | — |
| ~1.9 | LocalLow (Uzmi Games ToS 0.36, Actoz 0.34, …) | review |

## Creative apps

| GiB | What | Kind | Reclaim |
|---:|---|---|---|
| 12.0 + 3.4 + 0.6 | Adobe programs (Photoshop 2026, Substance 3D Painter, Acrobat), Common Files | programs | — |
| 2.4 + 0.84 | Adobe app data (UXP plugin storage 1.15, …) | app data | each app's preferences |
| 2.28 | Adobe Camera Raw (ProgramData): AI models, camera and lens profiles | profiles, not cache | new area `adobe/camera-raw` (uninstall) |
| 0.39 | Substance Painter crash dumps (`%LOCALAPPDATA%\CrashDumps`) | dumps | `windows/crash-dumps` (auto) |
| 17.6 | ZBrush QuickSave | owner data | owner (14) |
| 15.0 + 1.55 + 1.16 | Reallusion templates, ProgramData content manager, local data (RLDownloads installers 0.73) | content / app data / installers | installers: **new `creative-apps/reallusion-downloads`** (auto); the rest is the owner's |
| 3.24 | `C:\Autodesk\WI` (unpacked installers) + Autodesk in Program Files 1.8 + Common Files 0.9 | installer leftovers | **new `creative-apps/autodesk-installers`** (ask: Repair and some updates look for these files) |
| 4.23 | Unity ArtEngine (discontinued) | program | uninstall |
| 1.46 | Marvelous Designer assets | content | owner |
| 1.03 | Gaea autosaves | owner data | owner |
| 0.39 / 0.31 / 0.26 | Quixel Bridge cache, Replica Studios cache, DAZ 3D | cache / app data | — |
| 0.34 | Sony Imaging Edge models | app data | — |
