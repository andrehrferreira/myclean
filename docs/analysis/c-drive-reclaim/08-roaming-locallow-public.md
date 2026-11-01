# 08 ~\AppData\Roaming, LocalLow and Public (69.6 GiB)

## ~\AppData\Roaming: 32.8 GiB

| GiB | Folder | Date | What | Kind | Reclaim | Status |
|---:|---|---|---|---|---|:-:|
| 11.05 | `Code\WebStorage` | 2026-10 | Chromium storage buckets of VS Code webviews. `4\CacheStorage` 9.6 and `9\CacheStorage` 0.9 hold only `vscode-resource-cache-4/5/6`: copies of local files webviews showed. IndexedDB is beside them | cache | **new `vscode/webview-resource-cache`** (ask: a webview could keep its own caches in the same place) | 3 |
| 1.06 | `Code\CachedExtensionVSIXs` | | downloaded .vsix of installed extensions | download cache | `vscode/vsix-cache` (now auto, 30 days) | 1 |
| 0.43 | `Code\User` | | settings, workspace storage | app data | `vscode/workspace-storage` (ask, 90 days) | 3 |
| 4.23 | `.artomatix` | 2023-06 | Unity ArtEngine 2022.7.2 (discontinued), incl. a Unity viewer | program | new area `creative-apps/artengine` (uninstall) | 3 |
| 2.42 | `Adobe` | 2025-12 | UXP PluginsStorage (Photoshop) 1.15, CCX Welcome 0.34 | app data | review | – |
| 2.28 | `npm\node_modules` | 2026-10 | global npm packages (Codex CLI, …) | installed packages | `npm rm -g` | 3 |
| 2.09 | `nexa.ai\hyperlink` | 2025-11 | Nexa Hyperlink: engine 1.2 and index data | app data | new area `ai-tools/nexa-hyperlink` | 3 |
| 1.18 | `Cursor` | 2026-04 | `User\History` 0.68 (local file history), workspaceStorage 0.31 | app data | new area `vscode/cursor-data` | 3 |
| 1.03 | `QuadSpinner\Gaea\Autosave` | 2023-01 | terrain autosaves | owner data | new area `creative-apps/gaea-autosave` | 3 |
| 0.76 | `WhatsApp\Service Worker\CacheStorage` | 2022-12 | WhatsApp web app's offline cache | site data | — (folder unchanged since 2022; goes with the app) | 3 |
| 0.65 | `Slack\Service Worker` | 2022-04 | Slack's offline cache (folder unchanged since 2022) | site data | uninstall Slack | 3 |
| 0.52 | `Antigravity` | 2026-04 | Google Antigravity editor data | app data | — | – |
| 0.39 | `Bridge\Cache` | 2023-09 | Quixel Bridge cache | cache | — | 3 |
| 0.38 | `com.adobe.dunamis` | | Adobe telemetry queue | app data | — | – |
| 0.36 | `discord` | | Discord's Chromium caches and settings | cache + app data | `electron-apps/discord` (auto) for the caches | 1 |
| 0.33, 0.31, 0.26 | `Elgato`, `Replica Studios\Cache` (2023), `DAZ 3D` | | app data / cache | | — | 3 |

## ~\AppData\LocalLow: 1.9 GiB

Uzmi Games ToS 0.36 (2022), Actoz 0.34 (2023), Adobe 0.31, NVIDIA's per-driver shader caches
(`gpu/nvidia-*`, now auto). Area `windows/local-low` (review).

## C:\Users\Public\Documents: 34.8 GiB

| GiB | Folder | Date | What | Kind | Reclaim | Status |
|---:|---|---|---|---|---|:-:|
| 17.6 | `ZBrushData2025\QuickSave` | 2025-07 | ZBrush QuickSave files (`QS_*.zpr`, 0.5–1.0 each), the automatic saves of sculpts | owner data | new area `creative-apps/zbrush-quicksave`. Keep the wanted ones on another drive, delete the rest. ZBrush's QuickSave preferences limit how many it keeps | 3 |
| 0.67 | `ZBrushData2025\ZPluginData` | | plugin data | app data | — | – |
| 15.0 | `Reallusion\Reallusion Templates` | 2025-01 | Character Creator / iClone content templates | content library | new area `creative-apps/reallusion-content`: uninstall unused packs in Reallusion Hub | 3 |
| 1.46 | `MarvelousDesigner\Assets` | 2024-02 | asset library (with `AssetsMarvelous.part*.rar` archives) | content library | new area `creative-apps/marvelous-designer-assets` | 3 |
