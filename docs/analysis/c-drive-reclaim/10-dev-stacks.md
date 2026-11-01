# 10 Developer stacks on C: (across 05–09)

Everything development-related on C:, grouped by stack. Project workspaces are on F: and W:
(`data/workspaces.toml`). No `node_modules` of at least 256 MiB lies outside them on C:.

| Stack | GiB on C: | Where | Regenerable part and its case | Owner part |
|---|---:|---|---|---|
| WSL / Linux | 125.2 | Ubuntu-24.04 rootfs | `/tmp` 93.9 → `wsl-docker/tmp-prune` (new, auto) | the distribution (move it, 03) |
| Docker | 3.3 + 0.7 | Program Files, `%TEMP%\DockerDesktop`; `swap.vhdx` 6.5 while running | `docker system prune` (ask: it removes stopped containers) | — |
| Visual Studio | 15.7 + 4.8 + 10.3 + 1.3 | Program Files, x86, the package cache, `%LOCALAPPDATA%\Microsoft\VisualStudio` | package payloads 10.1 → `visual-studio/package-payloads` (new, auto); MEF cache (auto) | workloads, VS 2019 |
| Windows SDK / UWP | 5.2 + 5.5 | Windows Kits, Microsoft SDKs | — | 3 of the 6 SDKs, the UWP NuGet packages |
| .NET | 3.3 + 1.1 + 0.34 | dotnet, `~\.nuget\packages` | `dotnet nuget locals all --clear` (now auto) | SDK 6.0 and the 3.1/5.0 runtimes |
| CUDA / NVIDIA dev | 5.9 + 1.3 + 4.3 + 0.6 | CUDA, cuDNN, Nsight | — | old Nsight 2023 versions 1.8 |
| Android | 8.2 + 2.8 + 3.2 | `%LOCALAPPDATA%\Android\Sdk`, `.android`, the VS Android NDK | `android/sdk-temp`, `android/user-cache` (now auto) | system image 4.3, AVD 2.5, NDK |
| Rust | 11.4 + 2.1 + 1.2 | `.rustup`, `.cargo`, `ort.pyke.io` | `rust/registry-cache` (now auto), `target` of inactive projects (auto) | toolchains (move or compress) |
| Node / JS | 2.3 + 1.55 + 0.79 + 1.59 + 1.2 + 0.48 | npm global, npm cache, pnpm, Electron cache, Bun, nvm | npm/pnpm/yarn prunes (now auto), `electron-download-cache`, `bun-cache` (new, auto) | global packages, nvm versions |
| Python | 6.9 + 5.0 | Store Python 3.12 (user packages 6.0), the local AI runtime | `python-store/pip-cache`, `python/pip-cache`, `uv-cache` (now auto) | user packages |
| Java / Gradle | 7.2 + 0.6 | `.gradle`, two JDK 11 copies | `gradle-versions` (now auto) | JDKs |
| vcpkg | 3.1 + 2.0 (+ ~8 already in the quarantine) | `C:\vcpkg`, `~\vcpkg`, `%LOCALAPPDATA%\vcpkg` | `vcpkg/build-files`, `clone-build-files` (now auto) | installed libraries |
| Unreal | 2.7 | `%LOCALAPPDATA%\UnrealEngine` (Zen 1.6, DDC 0.8) | DDC relocation | — |
| Playwright | 1.45 + 3.3 | test browsers, MCP profiles | browsers (ask) | MCP profiles (logins) |
| Dart / Flutter | 0.79 + 0.30 | Pub cache, `.dartServer` | — (15) | — |
| Zig | 0.31 | global cache | `zig/global-cache` (now auto) | — |
| Trivy | 1.45 + 0.25 | `java-db`, `fanal` | `dev-tools/trivy-java-db` (new, auto) | — |
| AI assistants | 23.4 + 6.8 + 2.5 + 1.1 | `.codex`, `.claude`, `.cortex`, Claude Code versions | — | all (14) |
| Local AI models | 11.4 + 2.1 | `.elyriu-local-ai`, Nexa Hyperlink | — | all |

Not found on C:: `.m2`, conda environments, Unity caches, and Docker's or WSL 2's own disks
(none of at least 256 MiB).
