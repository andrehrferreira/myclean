# 08 Decisions for the owner

What only the owner can decide, largest first, with the GiB each frees on E: and the exact
action. Nothing here was done. "MyClean" says what MyClean runs once approved.

| # | GiB | Decision | Action | MyClean |
|---:|---:|---|---|---|
| 1 | 140.4 | **Path of Exile 2**: never started from Steam (`LastPlayed` 0), installed 2026-10-14 | Steam > Library > Path of Exile 2 > Manage > Uninstall; or Steam > Settings > Storage > move it to D: (reversible) | area `games/steam-library` (uninstall) |
| 2 | 82.5 | **Creature `Temp` folders** in Assets3D: Lich 62.2 (untouched since 2026-10-18), Barracoon 7.4, ChibiKnight 6.7 (in use), Undead Horse 6.1; plus `Assets3D\_tmp` 1.8 | Check that every correction is in the occurrence register (Assets3D rule), then delete the Lich `Temp` first; keep the current creature's | optional owner pack below (`ask`, quarantine) |
| 3 | 49.1 | **Recycle Bin**: `HiveLLM\Synap\target` 41.75 (2026-10-21), a scratch copy of UzPainter with its `.zig-cache` 6.5, small test folders, and 112 hidden `.<hex>` files without `$I` records (1.7, Feb–Apr; 04) | Approve the `windows/recycle-bin` plan (empties every drive's bin); optionally turn on Storage Sense's 30-day bin cleanup | handler `windows/recycle-bin` (ask) |
| 4 | 31.2 | **No Man's Sky** (last played 2026-09-20; a 7.5 GB update waits) | uninstall or move in Steam, as #1 | `games/steam-library` |
| 5 | 23.9 | **`UzPainter\scripts\tmp`** (104 run folders of 2026-10-23: pytest trees, build copies; 5.1 of it are duplicate binaries) | Grep the task notes and `tmp/claude-supervision*.json` for these paths (UzPainter's own rule), then delete the folder; its AGENTS.override.md already forbids creating it | — |
| 6 | 13.7 (+63.7 auto) | **Claude Code agent worktrees** of UzEngine: 8 registered, all branches merged into `main`; 6 clean, `agent-a1ea4dfd…` and `agent-aefaec15…` have 2 changed files each; `fix-vfx-pr7` (1.0) is not merged; 13 unregistered `agent-*` folders (0.1); `E:\tmp\uz-before` is listed as prunable | In `E:\UzmiGames\UzEngine`: `git worktree remove .claude\worktrees\agent-<id>` for the 6 clean ones; look at the 2 changed files of the others first; `git worktree prune`; delete the unregistered folders. Their caches and builds (49.5 + 14.2) go on their own anyway (02) | — (09-5) |
| 7 | 5.6 (+8.2 auto) | **`UzEngine\.sandbox\stabilization`** (verification checkouts and logs of 2026-10-10) | delete the folder, or keep the logs and JSON reports | — |
| 8 | 11.0 | **Painter and Blender backups** in Assets3D: autosaves, `UndeadHorse_paint.spp.tmpsave` (2.0), `.blend1` (7.1 together), and three identical `UndeadHorse_bake.spp` (3.9 extra) | Open each project once to confirm it is whole, then delete the autosaves and the `.tmpsave`; keep one `_bake.spp` | — |
| 9 | 18.3 | **TML compiler** (HiveLLM, last commit 2026-07-23): `build` 6.8, the `gcc` and `llvm-project` submodules (`.git\modules` 6.1, checkouts 5.4) | `git submodule deinit` the toolchain sources you no longer build; delete `build` (hours to rebuild LLVM) | — |
| 10 | 10.2 | **Expert** (idle since 2026-04-17): `venv_windows` 5.0, adapter weights 4.1, Qwen3-0.6B 1.1 | delete the venv (`pip install -r` recreates it); keep or archive the weights | — |
| 11 | 6.3 | **Datasets**: GloVe (2.1 text + the 0.8 zip it came from), SIFT 0.5, arXiv PDFs 2.0, `glove-100-angular` 0.45, Nexus `server-trace.log` 0.37 | delete `glove.6B.zip` at least; download again when a benchmark needs them | — |
| 12 | 8.1 + 0.85 | **UzEngine tooling**: `.screenshots` 2.8, `.rulebook\memory` 2.5 (March), `cache` 1.4, `.tools` 1.4, `runtime\thirdparty\.native.staging-*` 0.85 (installer leftovers) | delete old screenshots; rebuild or drop the March memory index; delete the staging folders (and fix `setup.ps1` to remove them) | — |
| 13 | 5.9 | **Elyriu** (untouched since 2026-07-22): server data 4.1, assets 0.9 | archive to an HDD (K: or L:) if paused | `node_modules` 0.74 is auto |
| 14 | 3.7 + 2.1 | **Falcor and Godot clones** (March) | delete; clone again when needed | — |
| 15 | 2.8 | `E:\tmp` (this week's agent scratch: a Cargo `--target-dir` 2.0, `myclean-verify`, `android-setup`) | delete when the work it belongs to is done | — |
| 16 | ~4.5 | **Compress** `.obj` meshes and dataset text (LZX) | — | compress analyzer / `CompressFolder` (status 2) |

## Optional: an owner pack for the creature scratch

Owner packs live in `%ProgramData%\MyClean\packs\<id>\` (administrators-only ACL, docs/packs.md)
and are never shipped. Drive letters are fine there. This one offers the creature `Temp`
folders untouched for 14 days as an `ask` plan, moving them to the quarantine, which keeps them
7 days for a restore:

```toml
# %ProgramData%\MyClean\packs\uzmigames\pack.toml
id          = "uzmigames"
version     = "1.0.0"
title       = "UzmiGames workspaces"
description = "Per-creature scratch folders of the Assets3D repository, offered for approval."
sources     = ["observed on reference machine: E:\\UzmiGames\\Assets3D\\.gitignore lists Creatures/*/*/Temp/ under Temporaries; AGENTS.md puts each creature's scratch in its Temp/"]
min_myclean = "0.1.0"

[applies_when]
any = [{ path_exists = 'E:\UzmiGames\Assets3D\Creatures' }]
```

```toml
# %ProgramData%\MyClean\packs\uzmigames\clean.toml
# Each entry of a creature's Temp untouched for 14 days; quarantined, so it can be restored for
# 7 days. Project, Models3D, Assembly and Concept are never targets.
[[rule]]
id       = "creature-temp"
category = "app-caches"
tier     = "ask"
scope    = "user"
strategy = "quarantine"
targets  = ['E:\UzmiGames\Assets3D\Creatures\*\*\Temp\*']
require  = { min_age_days = 14, not_in_use = true }
```

plus a fixture with `match` on `E:\UzmiGames\Assets3D\Creatures\Undead\Lich\Temp\pilot-parts`
and `no_match` on `…\Lich\Project` and `…\Lich\Temp`. Today it would offer 62.2 GiB (the Lich)
and 0.1 more.
