# 14 Decisions for the owner

These are the choices only the owner can make, with the GiB each frees on C: and the exact
action. The "MyClean" column says what MyClean runs once approved (plans from
`plans.propose_optimization` or the inbox). Otherwise the owner does it in the named tool.

| # | GiB | Decision | Action | MyClean |
|---:|---:|---|---|---|
| 1 | ~110 | **Move the page file** off C:. Don't shrink it: commit is 206 of 224 GB | System-managed page file on D: or W:; fixed 4–8 GiB on C: (or a `DedicatedDumpFile`) for the small crash dump; restart | setting `page_file` (area `windows/page-file`); undo: back to system-managed on C: |
| 2 | 25.4 / 50.9 | **Hibernation**: reduced (keeps Fast Startup) or off | `powercfg /h /type reduced`, or `powercfg /h off` | setting `hibernation` (`mode = reduced` / `off`); undo: `powercfg /h /type full` |
| 3 | 31.3 | **Move the WSL distribution** Ubuntu-24.04 to D:. Do it after the `/tmp` prune (93.9, auto) has run, so the export is 31 GiB, not 125 | `wsl --terminate Ubuntu-24.04`, then the recipe | recipe `wsl-docker/distro-files` (`wsl --export` → `wsl --import --version 1` → check → `wsl --unregister`); the archive is kept on D: as a backup |
| 4 | 4.7 + 7.1 + 3.3 + 2.5 | Inside WSL: rustup toolchains, CUDA 13.0 and Nsight in `/usr/local` and `/opt`, Ollama models, Swift | `rustup toolchain uninstall …`, `sudo apt remove …`, `ollama rm …` (in Linux) | — |
| 5 | 36 | **UzPainter work stores** (`oracle-work` 30.9, `oracle-inbox` 2.9, `parity-store` 1.9, smaller stores) | Archive or delete old envelopes, stability studies and runs from UzPainter; or move `%LOCALAPPDATA%\UzPainter` if the app has a setting for it | review only (pytest-temp 9.5 is auto) |
| 6 | 17.6 | **ZBrush QuickSaves** (`Public\Documents\ZBrushData2025\QuickSave`, last 2025-07) | Keep wanted sculpts on another drive and delete the rest; limit QuickSaves in ZBrush preferences | review only |
| 7 | 15.0 + 1.5 | **Reallusion templates** and Marvelous Designer assets | Reallusion Hub: uninstall unused content packs; MD: keep your garments elsewhere | review only |
| 8 | 22 | **Codex sessions** (17.9 + 0.6 archived), `thread_history_1.sqlite` 2.4, generated images 1.3 | Move `~\.codex\sessions\2025*` and old months to another drive, then delete them here; move the images worth keeping to Pictures | review only (never cleaned) |
| 9 | 6.6 | **Claude Code transcripts** (`~\.claude\projects`) | Lower `cleanupPeriodDays` in `~\.claude\settings.json` (Claude Code removes older transcripts itself) | review only |
| 10 | ~30 | **Uninstall**: Windows SDK 16299/19041/22621.2428 (~6.6); old CUDA/cuDNN (up to 7.2); VS workloads UWP and mobile (3.5 + 3.2 + part of VS); VS 2019 (≥1.1); Nsight 2023 (1.8); .NET SDK 6.0 and runtimes 3.1/5.0 (~1.2); two JDK 11 (0.6); ArtEngine (4.2); AmpliTube 5 and its zip (6.6) | Settings > Apps; VS Installer > Modify | uninstall areas |
| 11 | 18.2 | **Compress `Program Files (x86)`** (LZX) | — | `CompressFolder` (compress analyzer); undo: decompress |
| 12 | 11.4 | **Rust toolchains**: move, or compress (~5) | — | recipe `rust/rustup-home` (RUSTUP_HOME) or compress target `rust/toolchains` (LZX) |
| 13 | 7.2 | **Gradle home**: move | — | built-in relocation `gradle` (GRADLE_USER_HOME) |
| 14 | 10.5 | **VS Code webview cache** | — | approve the `vscode/webview-resource-cache` plan (ask) |
| 15 | 7.6 + 1.9 + 4.0 | **Chrome site data** and the on-device model | chrome://settings/content/all (clear the biggest sites); policy `GenAILocalFoundationalModelSettings = 1` | — |
| 16 | 8.8 + 9.2 + 2.4 | **Downloads** (Windows 11 ISO 8.1), Documents (AmpliTube zip 3.2, GoXLR 3.1, InstaLOD 1.4), Desktop | delete the ISO; move or archive the rest | — |
| 17 | 5.9 (+ 5.4) | **Local AI models**: nine `.incomplete` downloads from June; the models and the runtime | finish or cancel the downloads in the app that started them | review only |
| 18 | 4.3 + 2.5 + 2.1 | Android system image, AVD (snapshot 2.5), NDK | SDK Manager; Device Manager > Wipe data | review only |
| 19 | 6.0 | Store Python 3.12 `pip --user` packages | `pip list --user`, `pip uninstall …` | review only |
| 20 | 7.3 | **Restart Windows**: a cumulative update has waited since 2026-10-13, and there has been no restart since 2026-09-30 | Settings > Windows Update > Restart now | — |
| 21 | 3.2 | `C:\Autodesk` installers | approve the `creative-apps/autodesk-installers` plan, or move the folder to another drive | ask case |
| 22 | ~1–4 / < 1 | Component store, older driver packages | approve the DISM and `Device Driver Packages` plans | ask cases |
| 23 | ~25–30 (estimate) | Restore points (`System Volume Information`) | elevated: `vssadmin list shadowstorage`; cap it | setting `shadow_storage` |
| 24 | 3.3 / 2.3 / 2.5 / 2.1 / 1.2 | Playwright MCP profiles, global npm packages, Cortex memory, Nexa Hyperlink, Cursor data | delete in each tool | review only |
| 25 | 1.33 | 11 Windows Installer patches referenced by no product | leave (protected); a tool such as PatchCleaner can move them off | — |
