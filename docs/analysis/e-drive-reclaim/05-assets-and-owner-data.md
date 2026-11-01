# 05 3D assets, datasets, models and other owner data

What is left after the build output (02), the games (03) and the Recycle Bin (04) is the
owner's work. None of it is a clean case; this file says what it is and where the large
optional savings are.

## `E:\UzmiGames\Assets3D` (157.4 GiB)

The asset-production repository. Git holds only text (its `.gitignore`: "Models, projects,
textures, images … and intermediates stay on disk only"), so the disk copy is the only copy of
every model.

| GiB | What |
|---:|---|
| **82.5** | **`Creatures\<Family>\<Name>\Temp`**: per-creature scratch (AGENTS.md: "its scratch in its `Temp/`"; `.gitignore`: "Temporaries … `Creatures/*/*/Temp/`") |
| ↳ 62.2 | `Undead\Lich\Temp`: `painter-preview` 38.6 (Substance Painter projects, exports and maps per part, autosaves), `pilot-parts` 22.2, `robe-m3-probe` 1.3. Newest write 2026-10-18 (14 days); the Lich's Project and Assembly were last written 10-15 to 10-17 |
| ↳ 7.4 | `Humanoids\Barracoon\Temp` (newest 10-27) |
| ↳ 6.7 | `Humanoids\ChibiKnight\Temp` (`lowuv` 5.5; newest 10-30, in use this week) |
| ↳ 6.1 | `Undead\Undead Horse\Temp` (`painter-preview` 3.8 and 20 `hairproof-v*`/`floortest-v*` attempts; newest 10-25) |
| 68.0 | the creatures' `Project`, `Models3D`, `Assembly` and `Concept` folders: Undead Horse 25.9, Barracoon 17.1, Skeletal Mage 14.9, Lich 5.6, ChibiKnight 4.4. Every stage attempt (`Stages\NN-stage\aN`) keeps full copies of `.blend` and `.spp` files (e.g. 12 `assembly.blend` of 0.26 GiB in Barracoon's `01-assembled_high\a1…a12`) |
| 1.8 | `_tmp` (git-ignored "Temporaries": toolbox supervision runs, a copied UzEngine build 1.1) |
| 1.2 / 1.1 / 1.0 / 1.0 / 0.7 | `Unreal` (UE project, 0.14 of it is `Intermediate`/DDC), `Pipeline\panel\cache` (exported `.glb` previews), `Toolbox\out` (CMake output, 02), `Docs`, `Tools` |

The Temp folders are the single largest owner decision on E: after the games. They are
scratch by the repository's own rules, but its rules also say "Never leave a correction only
in Temp", so an agent or the owner may have parked something there that exists nowhere else.
That is why they are not `auto` and not even a shipped `ask` case: only this repository uses
this layout. 08 lists the decision and an optional owner pack that would offer them as an
`ask` plan.

The `.blend` and `.spp` files are already compressed by their applications (a zlib test on
the first 32 MiB: 0.98–1.00), so NTFS compression would gain nothing. Wavefront `.obj` meshes
(4.9 GiB, 6,007 files, mostly under `Stages\…\meshy`) compress to about a third.

## `E:\HiveLLM` (45.0 GiB, 24 projects)

| GiB | What | Last change |
|---:|---|---|
| 20.0 | `Tml` (compiler): `build` 6.8, submodule checkouts `src\llvm-project` 5.4 + `src\gcc` 0.9, `.git` 6.3 (submodule objects 6.1) | 2026-07-23 (commit) |
| 10.3 | `Expert`: `cli\venv_windows` 5.0 (PyTorch `torch\lib` 4.5), expert weights 4.1 (qwen3-0.6b adapters), `models\Qwen3-0.6B` 1.1 | 2026-04-17 |
| 5.3 | `Nexus`: `data\knn-corpora` 3.6 (GloVe text 2.1 **and** `glove.6B.zip` 0.8, the archive it was extracted from; SIFT 0.5), `gui\node_modules` 0.9, `bench-out` 0.4 (one `server-trace.log`) | 2026-09-21 |
| 3.3 | `Vectorizer`: two `node_modules` 1.9, `benchmarks\external\.work` datasets 0.7, `.git` 0.5 | 2026-09 |
| 2.0 | `CompressionPrompt\benchmarks\datasets\arxiv_pdfs` | 2026-04-17 |
| 4.4 | 19 smaller projects (HivehubCloud `.git` 0.9, Cortex, Synap, Transmutation, Rulebook …) | |

Status 3 except the `node_modules` of idle projects (02, existing auto rule). The datasets and
models were downloaded on purpose; the zip next to its extracted copy is the clearest
candidate (0.8 GiB). The GloVe and SIFT text files compress to about 40 % (status 2, ~1.3).

## Smaller owner folders

| GiB | What |
|---:|---|
| 5.9 | `E:\Elyriu` (untouched since 2026-07-22): `Dashboard\server\data` 4.1 (`sfproj` 2.2, `sheet-webp` 1.0, `genimages` 0.6 — generated images), `node_modules` 0.76 (auto, 02), `Assets` 0.9 |
| 3.7 / 2.1 | `E:\Falcor`, `E:\godot` (third-party clones, 02) |
| 2.8 | `E:\tmp` (agent scratch of this week, 02) |
| 0.26 | `E:\Options` (two small Node projects; `node_modules` 0.19 is auto) |
