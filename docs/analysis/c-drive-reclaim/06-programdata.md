# 06 ProgramData (35.9 GiB)

| GiB | Folder | Date | What | Kind | Reclaim | Status |
|---:|---|---|---|---|---|:-:|
| 10.3 | `Microsoft\VisualStudio\Packages` | payloads back to 2021 | VS Installer package cache: 1,494 `<package>,version=<v>` payload folders (10.1) beside `_Instances`, `_Channels`, `_bootstrapper` | installer download cache | **new `visual-studio/package-payloads`** (auto, service): payloads older than 30 days. "The installer will download them when needed" (Microsoft Learn). To stop it growing again: VS Installer > More > Settings > uncheck "Keep download cache" | 1 |
| 4.65 | `NVIDIA Corporation\NVIDIA App\UpdateFramework` | 2026-09/10 | `ota-artifacts\grd`: the 617.14 driver installer 0.74 + unpacked copy 2.9 (the installed driver is 617.14); `nvapp`: the NVIDIA app 11.0.9.565 installer + unpacked copy 0.86 | completed installer payloads (grd); a pending app update (nvapp) | **new `gpu/nvidia-app-downloads`** (auto, service, 14 days, not in use): driver packages only, 3.7. The app update `nvapp` (0.86; 11.0.9.565 downloaded, 11.0.9.251 installed) and the app's `status`, `registry` and `plugins` folders are never targets | 1 |
| 4.59 | `NVIDIA\NGX\models` | 2026-09 | NVIDIA Broadcast AI models (nvbcast 2.2, voice 0.7) and DLSS models (0.7 + 0.45) | app data | new area `gpu/ngx-models`: goes when NVIDIA Broadcast is uninstalled | 3 |
| 2.76 | `Package Cache` | | Installer copies for repair and uninstall | needed | protected | – |
| 2.67 | `Kaspersky Lab` | 2026-08 | AVP21.26 1.7 (databases, bases cache 0.5, webview2), KSDE5.26 0.8 (incl. `Temp\Setup` 0.26) | app data | new area `security-network/kaspersky-data` (none) | – |
| 2.28 | `Adobe\CameraRaw` | 2026-10 | ModelZoo 0.93 (AI models), CameraProfiles 0.70, LensProfiles 0.45 | profiles and models, not a cache | new area `adobe/camera-raw` (uninstall) | – |
| 1.92 | `Corsair\CUE5` | 2023-12 | iCUE GameSDKEffects 1.1 | app data | new area `device-software/corsair-icue` (uninstall) | 3 |
| 1.66 | `Epic\EpicGamesLauncher` | 2026-05 | `Data\ContentCache` 1.2, ManifestTemp 0.2 | launcher data | new area `games/epic-launcher-data` (uninstall) | – |
| 1.55 | `Reallusion` | 2026-09 | ContentManager MetaCache 1.0 (Character Creator 4: 0.6), CMS 0.3 | app data | new area `creative-apps/reallusion-data` (uninstall) | – |
| 0.87 | `ASUS` | 2026-02 | Armoury Crate Diagnosis 0.54 (MBLedSDK 0.39), FestsEffect 0.31 | app data | — | – |
| 0.65 | `Microsoft\Windows Defender` | | `Definition Updates\Backup` 0.43 is the previous definitions | Defender's | — | – |
| 0.39 | `Razer` | 2021-12 | Synapse 3 | app data | — | – |
| 0.34 | `Sony\Imaging Edge\OnnxData` | 2026-04 | Sony Imaging Edge AI models | app data | — | – |
| 0.27 | `NVIDIA Corporation\NVIDIA Broadcast` | | Broadcast data | app data | — | – |

Correction from review, about the NVIDIA case: only completed installer payloads are deleted.
The case targets only `ota-artifacts\grd\*`, the driver packages. Here that is the 617.14
package; driver 617.14 (32.0.16.1714) is installed. The NVIDIA app's own update (`nvapp`) is left
alone, because 11.0.9.565 was downloaded while 11.0.9.251 runs, so it may be waiting to install.
The 14-day age guard keeps an in-progress download, and `not_in_use` skips anything the app has
open. The rule cannot check whether the NVIDIA app is running (15).
