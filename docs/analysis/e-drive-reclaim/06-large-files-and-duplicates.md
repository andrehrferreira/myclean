# 06 Large files and duplicates

## Files of at least 256 MiB

73 files outside build folders (`ewalk.py`, 01). None is an installer, ISO, dump or archive
of something installed; they are the owner's working files:

| GiB | Files | Where |
|---:|---:|---|
| 22.6 | 330 `.blend` (largest 1.2) | Assets3D creatures, one per stage attempt |
| 20.2 | 27 `.spp` (largest 2.9) | Substance Painter projects of Undead Horse (`04-paint\a2…a5`, `03-bake\a12`) and Skeletal Mage |
| 2.0 | `UndeadHorse_paint.spp.tmpsave` (`04-paint\a4`, 2026-10-24) | Painter's temporary save: left when a save does not finish, so it may hold work the `.spp` beside it lacks (owner) |
| 2.5 | `UzEngine\.rulebook\memory\memory.hnsw` 1.47 + `memory.db` 1.03 (2026-03-24) | the rulebook memory of UzEngine, untouched for 7 months (owner) |
| 1.1 + 0.5 | `HiveLLM\Expert\models\Qwen3-0.6B\model.safetensors`, `Transmutation\models\layout_model.onnx` | models (owner) |
| 2.0 + 0.8 + 0.5 + 0.45 | GloVe text files, `glove.6B.zip`, SIFT, `glove-100-angular.hdf5` | benchmark datasets (owner) |
| 0.63 | `UzEngine\sample\content\meshes\sponza.glb` | sample content, tracked |
| 0.37 | `HiveLLM\Nexus\bench-out\phase8\server-trace.log` (2026-07-13) | a benchmark log (owner) |

Status 3 throughout. None is a clean case: they are documents of their applications.

Application backups inside `E:\UzmiGames` (a `find` by name): Substance Painter autosaves
(`*_autosave_*.spp`), temporary saves (`*.spp.tmpsave`) and Blender's previous-save copies
(`*.blend1`) add up to about **7.1 GiB in 32 files** (Blender backups 0.8 of it). They are
recovery copies the applications keep beside a project; useful only after a crash, but only the
owner knows whether one was needed. Owner (08), not a rule.

## Exact duplicates (files of at least 32 MiB)

Every file of at least 32 MiB outside `.git`, `node_modules`, `.zig-cache`, `zig-cache`,
`zig-out`, `build`, `target`, the Steam library and the Recycle Bin, grouped by size and then
by a BLAKE2 hash of the whole content (`dup2.py`, read-only). **150 groups, 23.0 GiB of extra
copies.**

| Extra GiB | Where | What |
|---:|---|---|
| 11.9 | `Assets3D\Creatures` | `UndeadHorse_bake.spp` ×3 (`04-paint\a3`, `a4`, `03-bake\a12`: 3.9 GiB extra), Lich `Temp\painter-preview\11-Robe-v*\work\…High-world.blend` ×8 (0.8), baked normal maps copied between `Temp\pilot-parts` and `Temp\painter-preview`, `placed.blend` of two fit attempts |
| 5.1 | `UzPainter\scripts\tmp` | the same `UzPainter.exe`/`.pdb`, test executables and `embree4.dll` (×37 with `thirdparty\embree`, 1.3) in many run folders |
| 2.5 | `UzEngine\runtime\thirdparty` | `.native` and two `.native.staging-*` folders (0.43 GiB each) with the same FFmpeg DLLs: the staging copies of the native dependency installer were never removed (a third, `.native.staging-spvfw9rc`, is 12 MiB) |
| 1.8 | `UzEngine\sample` and worktrees | tracked sample content (HDRIs, VFX textures) checked out in each worktree and the sandbox |
| 1.3 | `UzPainter\thirdparty` | see the embree row |
| 0.4 | `UzEngine\.tools`, `HiveLLM\Expert\experts`, `.sandbox` | small |

Most of these go with folders already discussed: the Temp folders and `scripts\tmp` (owner,
08), the worktrees (owner, 08). The ones that do not:

- **`UndeadHorse_bake.spp` ×3 (3.9 GiB)**: three byte-identical 2 GiB Painter projects in three
  stage attempts. Keeping one (or replacing two by hard links) frees 3.9 GiB. Owner: the
  pipeline may expect each attempt folder to be complete.
- **`UzEngine\runtime\thirdparty\.native.staging-*` (0.85 GiB)**: leftovers of the project's
  own installer (`setup.ps1`). Owner, or a fix in that script.

MyClean's dedupe proposal (`scan-all`, `dedupe`) would find the same groups; replacing copies by
hard links is not safe for files an application rewrites in place (Painter and Blender save by
writing a new file and renaming, which breaks the link but keeps the data; other tools may
not), so it stays an owner decision.

## Compression (status 2)

| GiB | Content | Expected saving |
|---:|---|---:|
| 4.9 | `.obj` meshes in Assets3D (zlib on the first 32 MiB: 0.33) | ~3.2 |
| 2.1 | GloVe text (0.38) | ~1.3 |
| 22.6 + 20.2 | `.blend`, `.spp` (0.98–1.00) | none |
| 23.7 | `.png` (already compressed) | none |

About **4.5 GiB** in all. The compression analyzer applies its own exclusions (recent files),
so it would offer less while the creatures are being worked on.
