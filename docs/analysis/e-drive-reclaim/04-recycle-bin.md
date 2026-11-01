# 04 Recycle Bin

`E:\$RECYCLE.BIN` uses **49.1 GiB** (50.2 GiB of file bytes), all in the owner's bin
(`S-1-5-21-…-1001`). The system bin `S-1-5-18` cannot be read without admin and is part of the
4.0 GiB "not visible" (01).

The bin's `$I` files (one per deleted item: original path, size, deletion time) were read
directly; nothing was opened in Explorer.

| GiB | Original path | Deleted | What it is |
|---:|---|---|---|
| 41.75 | `E:\HiveLLM\Synap\target` | 2026-10-21 | Cargo build output (`debug\incremental` 38.0, `release` 2.7) |
| 6.50 | `E:\UzmiGames\uv-ef618d43` | 2026-10-24 | a scratch copy of the UzPainter repository with its `.zig-cache` (6.3) |
| 0.19 | (no `$I` record) | 2026-10-21 | another repository copy (`adapters`, `assets`, `docs`) |
| ~0 | 7 small items | 2026-10-24 to 10-27 | pytest `test_*0` folders of 2026-10-23 runs, `Assets3D\…\ChibiKnight\Project` (a `Panel` folder and `production.json`) |
| 1.71 | 112 hidden files named `.<hex>` (e.g. `.00040000000d81ba3d4f764b54fb9549`), no `$I` record, written 2026-02-20 to 04-18 | | not Explorer deletions. This is how Cygwin/MSYS tools (Git Bash) get rid of a file that is still open; an assumption, not checked |
| **50.2** | | | |

684 more `$I` records have no `$R` content any more: Windows removed the content (the bin is
over its size limit or was partly emptied). Their recorded sizes add up to 75 GiB, e.g.
`E:\Elyriu\Dashboard\tools\comfyui\data` (23.5, 2026-06-14) and four `E:\Build\Build2022-02-*.rar`
(4.1 each); they use no space now.

**Status 3 (owner).** 48.3 of the 50.2 GiB is build output or a copy of a repository, so
emptying the bin loses almost nothing that cannot be rebuilt (the small ChibiKnight `Project`
folder is the exception to look at). But the bin is the owner's undo, and the shipped
`windows/recycle-bin` handler is `ask` on purpose (C: analysis, 13). Two ways:

1. Approve the `windows/recycle-bin` plan once (49.1 GiB now; it empties every drive's bin).
2. Turn on Storage Sense's "Delete files in my recycle bin if they have been there for over
   30 days" (Settings > System > Storage > Storage Sense). It applies to every fixed drive. It
   would remove none of the items above yet (the oldest deletion is 2026-10-21); whether it
   touches the 112 hidden files, which have no deletion record, is not known.

An age-based rule ("bin items deleted more than 30 days ago") would fit the auto tier, but the
deletion time is only in the `$I` record, not in the file times the engine compares: it needs
code (09).
