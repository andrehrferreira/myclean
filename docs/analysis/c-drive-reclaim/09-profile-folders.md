# 09 The profile's own folders (~, outside AppData: 99.8 GiB)

| GiB | Folder | Date | What | Kind | Reclaim | Status |
|---:|---|---|---|---|---|:-:|
| 23.4 | `.codex` | 2026-10 | `sessions` 17.9 (single rollouts up to 1.5 GiB), `archived_sessions` 0.6, `thread_history_1.sqlite` 2.4, `generated_images` 1.3, plugins 0.45, sandbox bin 0.34 | owner data (assistant sessions) | `ai-tools/codex-sessions` (review, never cleaned). Archive old months to another drive (14) | 3 |
| 11.4 | `.rustup` | | Rust toolchains | toolchains | `rust/rustup-home` (move, RUSTUP_HOME) **or** `rust/toolchains` LZX compression; `rustup toolchain uninstall` | 2 |
| 11.4 | `.elyriu-local-ai` | 2026-06 | `models` 6.4: Qwen-Image-Edit 4.0, HiDream-I1-Dev 2.4. **5.9 of it is nine `*.incomplete` blobs**, downloads interrupted in June. `runtime` 5.0 (Python with torch CUDA) | owner data (models) | `ai-tools/local-ai-models` (review). Resume or abandon the downloads in the app; the `.incomplete` files are not usable models | 3 |
| 9.2 | `Documents` | | IK Multimedia 3.2 (incl. the 3.2 GiB `AmpliTube_5_10_9.zip` installer), GoXLR 3.1 (2026-02), InstaLOD Studio 1.4, recordings 0.7, AutoRigPro 0.4 | owner data + an installer archive | deny-listed known folder: owner only | 3 |
| 8.8 | `Downloads` | | **`Windows11_Client_x64_pt-br_26300_9457.iso` 8.1** | owner data (re-downloadable ISO) | owner: delete or move the ISO | 3 |
| 7.2 | `.gradle` | 2026-10 | `caches` 6.4 (`9.3.1` 5.0 incl. three identical 0.33–0.38 GiB transforms, `modules-2` 1.5), `wrapper\dists` 0.7 | cache | `java-gradle/gradle-versions` (now auto, 90 days unused); move GRADLE_USER_HOME (built-in `gradle` relocation) | 1 / 2 |
| 6.8 | `.claude` | 2026-10 | `projects` 6.6 (UzmiGames Assets3D 2.6, UzPainter 1.9, …) | owner data (transcripts) | `ai-tools/claude-projects`: `cleanupPeriodDays` in settings.json | 3 |
| 3.0 | `.cache` | | `ldbc-snb` 1.6 (an LDBC benchmark dataset, sf1 1.2, from 2026-07), `codex-runtimes` 1.3 | dataset + runtime | `dev-tools/user-cache` (review) | 3 |
| 2.8 | `.android` | 2026-10 | `avd\ar_pixel.avd` 2.5, of which the quick-boot snapshot `ram.img` is 2.5 | emulator state | Device Manager > Wipe data / cold boot | 3 |
| 2.5 | `.cortex` | 2026-11 | `local` 1.8 (segments, index.sqlite3 0.96), `archive\events` 0.5 | agent memory | new area `ai-tools/cortex-memory` | 3 |
| 2.4 | `Desktop` | | `Ar Online` 1.9 (incl. a 1.1 GiB zip), a 2024 folder 0.45 | owner data | — | 3 |
| 2.3 | `.vscode\extensions` | 2026-10 | ChatGPT 1.6, Claude Code 2.1.314 and **2.1.312** (an old version left, 0.29) | extensions | VS Code removes superseded versions after a restart | 3 |
| 2.1 | `.cargo` | | `registry\src` 1.3, `registry\cache` 0.26, `bin` 0.27 | cache + tools | `rust/registry-cache` (now auto, 90 days) | 1 |
| 1.2 | `.bun` | | `install\cache` 1.06, `bin` 0.2 | cache | **new `node/bun-cache`** (auto, 30 days) | 1 |
| 1.1 | `.local` | 2026-10 | `share\claude\versions` 0.69 (Claude Code builds), `bin` 0.37 | tools | new area `ai-tools/claude-code-versions` | 3 |
| 0.73 | `swift-6.0.3-RELEASE-ubuntu24.04.tar.gz` | | a Swift download (it is installed in WSL) | download | owner | 3 |
| 0.58 | `.docker` | | `scout\sbom` 0.47 | cache | — | 3 |
| 0.50 | `neo4j` | | Neo4j data (transactions) | owner data | — | 3 |
| 0.49 | `.git\lfs\objects` | | a Git repository at the profile root, with LFS objects | owner data | — | 3 |
| 0.43 | `.3T\updates` | | Studio 3T updates | installer | — | 3 |
| 0.30 | `Ubiquiti UniFi` | | UniFi controller database | owner data | — | 3 |

Under 256 MiB: 1.3 GiB.
