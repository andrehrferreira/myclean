# 05 Toolchains, servers and the rest (19.1 GiB)

| GiB | Folder | Notes | Status |
|---:|---|---|:-:|
| 7.01 | `F:\Python313` (Python 3.13.9, Uninstall key without InstallLocation, no PEP 514 key) | site-packages 6.6: **pip leftovers 3.79** (below), torch 2.12.1+cpu 0.5, opencv, pandas, … (newest install 2026-10-31) | 1 / 2 |
| 3.86 | `F:\LLVM` (2026-04-14) | bin 3.2, lib 0.57; `clang.exe` sample compresses to 55 % | 2 (~1.7) |
| 2.94 | `F:\MongoDB\Server\8.0` | service `MongoDB` stopped (start type Automatic), no Uninstall key; bin 2.01 of which `mongod.pdb` 1.1 and `mongos.pdb` 0.7; `log\mongod.log` 0.51 (last write 2026-10-14, compresses to 13 %); data 0.42 | 3 |
| 1.09 | `F:\IDEs\Microsoft VS Code` (user install 1.144, one version folder `2c33b50b16`) | the program | – |
| 1.00 | `F:\PostgreSQL\18` | service `postgresql-x64-18` running; **its own pgAdmin 4 0.66**, data 0.08, bin 0.08 | – |
| 0.77 | `F:\pgAdmin 4` (pgAdmin 4 9.9, installed 2025-11-05) | a second pgAdmin next to PostgreSQL's | 3 (0.66–0.77) |
| 0.50 | `F:\$RECYCLE.BIN` | owner's bin: 266 files deleted from 2022-10-02 to 2026-10-30 (e.g. `F:\Node\ucsjs\…`, `F:\Node\tcg-classify\.cursor`); SYSTEM's bin empty | 3 |
| 0.20 | `F:\Go` (Go source tree of 2025-10-31) | | – |
| 0.14 | `F:\Python312` (3.12.9, PEP 514 key points here) | | – |
| 0.07 | OndeSaiMaisBarato 0.05, Protoc 0.01, Falcor (11 files), `CC4\Reallusion Templates` (empty, one unreadable entry), `.pnpm-store` | | – |

## pip's uninstall leftovers (3.79 GiB, auto)

| GiB | Folder | Files | Last write |
|---:|---|---:|---|
| 3.505 | `F:\Python313\Lib\site-packages\~-rch` (`lib\torch_cuda.dll` 0.9, cuDNN, cuBLAS, cuFFT …) | 38 | 2026-06-25 19:14 |
| 0.260 | `…\site-packages\~orch` | 10 | 2026-06-25 18:54 |
| 0.020 | `…\site-packages\~umpy.libs` | 2 | 2025-11-05 |
| 0.004 | `…\site-packages\~umpy` | 2 | 2026-06-25 19:14 |

When pip uninstalls a distribution (also as the first half of an upgrade), it does not delete
the files at once: `StashedUninstallPathSet` moves each top-level folder aside into an
`AdjacentTempDirectory`, whose name replaces the leading characters of the original with `~`
and then `-~.=%0-9` ("valid filesystem characters, but not valid package names"): `torch` →
`~orch`, then `~-rch`. On commit pip deletes the stash; on failure it moves it back. On
Windows the delete fails when a DLL of the package is loaded by another process, and the stash
stays forever; pip then warns `Ignoring invalid distribution ~orch` on every run. (Source:
pip's `src/pip/_internal/utils/temp_dir.py`, `AdjacentTempDirectory._generate_names`, and
`src/pip/_internal/req/req_uninstall.py`, `StashedUninstallPathSet`.)

Here: a CUDA build of torch was stashed at 19:10–19:14 on 2026-06-25 and torch 2.12.1+cpu was
installed at 19:38. The stash is a dead copy that Python cannot import (a module name cannot
start with `~`) and pip ignores. A folder that is a week old cannot belong to a running pip.
So it is 100 % safe: new rule `python/pip-uninstall-leftovers` (06), tier auto, 7 days, not in
use.

**But it cannot reach `F:\Python313` without a code change.** The python.org installer puts a
per-user install in `%LOCALAPPDATA%\Programs\Python\Python3XY` by default; this one was
installed to a custom folder at the root of F:, which no variable names (the 3.13 install has
no PEP 514 key either). Clean targets need a real drive or a variable: a `?:\Python3*\…`
target loads but the engine drops it ("a target must be an absolute path"). The two ways are a
drive-letter target `F:\Python3*\Lib\site-packages\~*` (only valid on this machine: not for a
shipped pack) or teaching the engine `?:\` = every fixed drive (06). With neither, the rule
frees 0 on F: and works for the standard folders only.

## Compression (status 2)

LZX would save ~1.7 GiB on LLVM, ~1.3 on what is left of Python313 after the stash goes
(binaries ~40 % smaller, estimate) and ~0.8 on MongoDB's bin (`mongod.pdb` sample 59 %). The
compression analyzer handles these folders as it finds them; no pack target is needed.

## MongoDB (2.94 GiB, owner)

The server is stopped and was installed without an Uninstall entry (MongoDB Compass, on C:,
is a separate program). If it is no longer used: stop the service from starting
(`sc.exe config MongoDB start= disabled`, elevated), then delete it (`sc.exe delete MongoDB`)
and the folder, after exporting what `data` holds (0.42). If it is used: the two `.pdb` files
(1.8) are debug symbols only needed to read a crash dump, and `mongod.log` (0.51) is one log
never rotated (`db.adminCommand({ logRotate: 1 })`, or `systemLog.logRotate` in `mongod.cfg`).
Not a pack case: MongoDB is installed wherever the owner put it.

## Two pgAdmins (0.66–0.77 GiB, owner)

PostgreSQL 18's installer added its own `pgAdmin 4` (0.66) under `F:\PostgreSQL\18`, and a
standalone pgAdmin 4 9.9 is in `F:\pgAdmin 4` (0.77). One is enough: uninstall the standalone
one (Settings > Apps > "pgAdmin 4 version 9.9"), or deselect pgAdmin in PostgreSQL's
installer (Modify).

## `F:\.pnpm-store` (12 KiB)

Created tonight at 00:46:11, holding only `v11\index.db`. pnpm keeps one store per drive: when
the working folder is on another drive than the default store, it uses (and creates)
`<drive>:\.pnpm-store` (pnpm docs, `store-dir`). So something ran pnpm with its working folder
on F: at 00:46, three minutes before the C: cleaning run. MyClean's command runner passes no
working folder (`CreateProcessW(…, lpCurrentDirectory = nullptr, …)`,
`src/platform/cleanup/win32_command_runner.cpp:269`), so a native `pnpm store prune` inherits
the caller's: started from `F:\MyClean\build\…`, it prunes an empty F: store and leaves the
real one untouched. Not proven that it was MyClean, but the fix is cheap (06, code changes).
