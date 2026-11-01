# 12 Large files and duplicates

## Files of at least 500 MB (69 in the map)

| GiB | File | Type | Reclaim |
|---:|---|---|---|
| 119.06 | `C:\pagefile.sys` | page file | move (02) |
| 50.88 | `C:\hiberfil.sys` | hibernation | reduce or off (02) |
| 8.13 | `~\Downloads\Windows11_Client_x64_pt-br_26300_9457.iso` | Windows ISO | owner: delete (re-downloadable) or move |
| 6.54 | `~\AppData\Local\Temp\<GUID>\swap.vhdx` | WSL 2 VM swap disk (in use) | goes when the VM stops; `temp-user` takes it then |
| 3.20 | `~\Documents\IK Multimedia\IK Product Manager\AmpliTube 5\AmpliTube_5_10_9.zip` | installer archive | owner |
| 2.50 | `~\.android\avd\ar_pixel.avd\snapshots\default_boot\ram.img` | emulator quick-boot snapshot | Device Manager (cold boot) |
| 2.36 | `~\.codex\thread_history_1.sqlite` | Codex history database | owner |
| 2.16 + 1.15 | `C:\Program Files\IK Multimedia\AmpliTube 5\AmpliTube 5.pak`, `…app.pak` | program | — |
| 2.07 | `…VisualStudio\Packages\Microsoft.VisualStudio.VC.Llvm.Clang,…\payload.vsix` | VS payload | `visual-studio/package-payloads` (auto) |
| 1.97 + 6 more (5.9 total) | `~\.elyriu-local-ai\models\…\blobs\*.incomplete` | interrupted model downloads (June) | owner |
| 1.53, 0.60, 0.54, 0.51, 0.47 | `~\.codex\sessions\2026\10\…\rollout-*.jsonl` | session transcripts | owner |
| 1.45 | `~\AppData\Local\java-db\trivy-java.db` | Trivy DB | `dev-tools/trivy-java-db` (auto) |
| 1.14 + 0.56 | `~\AppData\Roaming\.artomatix\…\Artomatix.ArtEngine.exe`, `cudnn_cnn_infer64_8.dll` | ArtEngine | uninstall |
| 1.08 | `~\Desktop\Ar Online\DPTO JRD DASH.zip` | archive | owner |
| 1.03, 0.90, 0.70 ×4, 0.68 ×6, 0.63 ×2, 0.58, 0.55, 0.51, 0.47 | `C:\Users\Public\Documents\ZBrushData2025\QuickSave\QS_*.zpr` | QuickSave | owner |
| 0.96 | `~\.cortex\local\index.sqlite3` | agent memory | owner |
| 0.89, 0.61, 0.55 | `~\.elyriu-local-ai\runtime\…\torch\lib\*.dll/.lib` | Python runtime | owner |
| 0.77 + 0.74 | Docker Desktop's `docker-wsl-cli.iso`, `docker-desktop.iso` | program | — |
| 0.74 | `…NVIDIA App\UpdateFramework\ota-artifacts\grd\…\617.14-…-whql-g.exe` | driver installer | `gpu/nvidia-app-downloads` (auto) |
| 0.73 | `…VisualStudio\Packages\AndroidNDK_R23C,…\android-ndk-r23c-windows.zip` | VS payload | auto |
| 0.73 | `~\swift-6.0.3-RELEASE-ubuntu24.04.tar.gz` | download | owner |
| 0.71 | `C:\Windows\SoftwareDistribution\Download\4e05f6…` | update payload | restart (04) |
| 0.71, 0.70, 0.60 | `C:\Windows\Installer\39e10a6a.msp` (unreferenced), `c3751b2.msi`, `142eedc8.msi` | Installer cache | protected (04) |
| 0.65 | `C:\Program Files (x86)\ASUS\AsusGamingCenterInstallPackage\…msixbundle` | installer bundle | — (deny-listed) |
| 0.63 | `~\.claude\projects\e--UzmiGames-Assets3D\…jsonl` | transcript | owner |
| 0.58, 0.52 | `~\AppData\Local\b1\Saved\…ushaderprecache`, `Wardogs\Saved\…` | game shader byte code | `games/unreal-game-shader-precache` (auto) |
| 0.52 | `~\AppData\Roaming\nexa.ai\hyperlink\hyperlink_data\6362ecf71fffaa8e` | index | owner |
| 0.51 | `~\AppData\Roaming\Code\CachedExtensionVSIXs\openai.chatgpt-…` | VSIX | `vscode/vsix-cache` (auto) |
| 0.50 | `C:\Windows\System32\DriverStore\FileRepository\awcc_…\setup.exe` | driver package | — |
| 0.49 | `C:\Program Files (x86)\Google\GoogleUpdater\crx_cache\…` | updater cache | — (deny-listed) |
| 0.49 | `C:\Users\Public\Documents\MarvelousDesigner\Assets\AssetsMarvelous.part1.rar` | archive | owner |
| 0.49, 0.48, 0.48 | `accurig-updater\installer.exe`, `RLDownloads\ActorCore_AccRIG.exe`, `actorcore-accurig-updater\installer.exe` | installers (the same AccuRIG setup three times) | auto (electron-updater, Reallusion downloads) |
| 0.48 | `C:\Program Files\Common Files\Adobe\Acrobat\Setup\{AC76BA86-…}\Core.cab` | Acrobat repair source | — |

## Duplicates (scan-all: 9 sets of at least 1 MiB, 4.4 GiB redundant, 8 GiB hashed of the budget)

| GiB redundant | Copies | Where | Reclaim |
|---:|---|---|---|
| 1.14 | 4 × 390 MiB | UzPainter `oracle-work\envelopes\painter-12.2.0…\runs\…` | owner (UzPainter's own stores) |
| 0.55 | 2 × 562 MiB | Store Python `local-packages` vs `.elyriu-local-ai\runtime` (a torch library) | owner |
| 0.50 | 2 × 515 MiB | inside the WSL rootfs | never from Windows |
| 0.48 | 2 × 495 MiB | `actorcore-accurig-updater\installer.exe` = `Reallusion\RLDownloads\ActorCore_AccRIG.exe` | both now auto cases |
| 0.37, 0.36, 0.33 | 2 each | `.gradle\caches\9.3.1\transforms\…` (Android native libraries transformed twice) | Gradle's own cache |
| 0.31 | 2 × 319 MiB | `npm\node_modules\@openai\codex\…\codex.exe` = `.codex\.sandbox-bin\codex.exe` | Codex's |
| 0.31 | 2 × 318 MiB | `OpenAI\Codex\bin\…\codex.exe` = `.codex\plugins\…` | Codex's |

565 more hashes did not fit the 8 GiB hashing budget. No duplicate set is worth a dedupe plan
beyond what the cases above remove.
