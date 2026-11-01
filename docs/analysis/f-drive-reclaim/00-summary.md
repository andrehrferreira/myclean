# F: drive reclaim: summary

**Answer.** F: ("WS", 1,863 GiB NVMe, 1,678 GiB used, 184.6 GiB free = 9.9 %) is almost all
two things: a **998 GiB copy of Docker Desktop's data disk** that Docker no longer uses
(`F:\WSL\wsl\DockerDesktopWSL.ANTIGO`, untouched since 2026-09-11; Docker runs from
`D:\Database\DockerDesktopWSL`), and a **561 GiB election-audit dataset** in which the TSE zips
(204 GiB) and their extracted copy (230 GiB, the ~2 million small folders) hold the same files.
Only the owner can decide on either. MyClean can delete **~3.8 GiB on its own** (pip's
uninstall leftovers, after a small code change), compress **~25 GiB** reversibly, and the
owner's decisions free **up to ~1,294 GiB**. Deleting the Docker copy alone takes F: from 9.9 %
to 63.5 % free.

Measured read-only on 2026-11-01. The `map` ran at 08:22. Sizes are GiB from that map unless
marked otherwise.

## Where the 1,678 GiB go, and what to do

The status column has three values: **(1)** MyClean deletes it on its own (tier `auto`, 100 %
regenerable), **(2)** reversible move, reduction or compression, **(3)** only the owner decides.
"–" means it is needed as it is.

| GiB | What | Action | Status | Reclaim GiB |
|---:|---|---|:-:|---:|
| 998.4 | `F:\WSL\wsl\DockerDesktopWSL.ANTIGO\disk\docker_data.vhdx`: an old copy of Docker Desktop's data disk, last written 2026-09-11, opened by nobody | delete the whole `F:\WSL` folder after checking the D: disk works (02) | 3 | 998.4 |
| 0.56 | two docker-desktop `ext4.vhdx` files under `F:\WSL\wsl` that no WSL distribution uses | go with the folder above | 3 | 0.56 |
| 230.1 | `…\auditoria-2turno-eleicao2026\data\raw\{1t,2t}\urnas`: the zips extracted per section (≈1 M sections, ~2 M folders, ~6 M files) | delete it **or** the zips below, not both; the project's `node src/coleta.js` extracts again (03) | 3 | 230.1 (+~10 NTFS) |
| 204.2 | `…\data\raw\{1t,2t}\zip`: the 2026 TSE zips (2t 115.6, 1t 88.5) | the alternative to the line above | 3 | (204.2) |
| 39.1 | `…\data\raw\2022\{1t,2t}\zip`: 2022 TSE zips | owner: keep while the 2022 comparison is in use | 3 | 39.1 |
| 11.8 | `…\data\db\leitura\eleicao2026-20261029T092711.sqlite`: a read copy that `atual.json` no longer points to | the project's own `node src/banco-leitura.js --remover-antigas` | 3 | 11.8 |
| 38.0 | the rest of the project's database: primary 17.9 + WAL 1.0, current read copy 19.0 | – (in use) | – | |
| 30.1 | `…\data\parsed` (27.7 GiB of JSONL) | LZX compression once the files stop changing (samples compress to 12–25 %) | 2 | ~22 |
| 7.6 | `…\data\raw\dadosabertos` (zips) 7.2, `manifest` 0.4 | – (owner's data) | – | |
| 76.7 | `F:\MyClean\build`: 63 build configurations; 55 of them (56.8 GiB) not built for 18–29 h, 8 belong to running agents (19.9) | not proposed: the repo is in active use; drop finished agents' configurations when their work ends (04) | – | (56.8) |
| 8.7 | `F:\MyClean-deploy\build` (debug 5.0, release 3.5) | – (the deploy worktree, building now) | – | |
| 3.8 | `F:\Python313\Lib\site-packages\~-rch`, `~orch`, `~umpy`, `~umpy.libs`: pip's uninstall stash of torch and numpy, left 2026-06-25 | new `python/pip-uninstall-leftovers` (auto); reaches F: only with `?:\` targets (06) | 1 | 3.8 |
| 3.2 | the rest of `F:\Python313` (torch 2.12.1+cpu, opencv, …) | LZX (binaries ~40 % smaller, estimate) | 2 | ~1.3 |
| 3.9 | `F:\LLVM` (installed 2026-04) | LZX (`clang.exe` sample: 55 %) | 2 | ~1.7 |
| 2.9 | `F:\MongoDB\Server\8.0`: service stopped; `mongod.pdb`/`mongos.pdb` 1.8, `mongod.log` 0.5, data 0.4 | uninstall if unused; else delete the two .pdb debug symbol files and rotate the log; or LZX the bin folder (~0.8) | 3 | 2.9 |
| 1.8 | `F:\pgAdmin 4` 0.77 (standalone 9.9) and `F:\PostgreSQL\18` 1.0, which bundles a second pgAdmin 4 (0.66) | uninstall one of the two pgAdmins | 3 | 0.7 |
| 1.1 | `F:\IDEs\Microsoft VS Code` (user install 1.144) | – | – | |
| 0.5 | `F:\$RECYCLE.BIN` (266 files deleted 2022 – 2026-10-30) | empty it (existing `windows/recycle-bin` case, ask) | 3 | 0.5 |
| 0.4 | Go 0.20, Python312 0.14, OndeSaiMaisBarato 0.05, Protoc, Falcor, CC4, `.pnpm-store` | – | – | |
| 15.5 | not visible: NTFS overhead of the ~6 M small files (cluster slack ~10, MFT records ~7; estimate) and System Volume Information (needs admin) | most of it goes with the `urnas` tree (counted above as +~10) | 3 | (in urnas) |

## Totals

| | GiB on this drive |
|---|---:|
| **(1) MyClean deletes on its own** (pip uninstall leftovers 3.8; needs `?:\` in clean targets to reach `F:\Python313`, otherwise 0 on F:) | **~3.8** |
| **(2) reversible: compress** (parsed JSONL ~22, LLVM ~1.7, Python ~1.3; MongoDB is counted under (3)) | **~25** |
| **(3) owner decision** (Docker disk copy 999.0, extracted urnas 230.1 + ~10 NTFS, 2022 zips 39.1, old read copy 11.8, MongoDB 2.9, pgAdmin 0.7, Recycle Bin 0.5) | **~1,294** |
| not counted: MyClean's own build output (85.4), which comes back when the current work ends | |

The totals don't overlap. The zips and the extracted tree are alternatives: the larger one
(the extracted tree) is counted. MongoDB is counted once, as an uninstall.

## Why so little is automatic here

F: holds no app caches: every cache folder of the packs lives under the profile on C:. What is
on F: is one VM disk, one dataset, two source repos with their builds, and toolchains
installed in custom folders. None of the 20 project-artifact rules hits anything on F:, and
correctly so (04): the only artifacts in a git work tree belong to MyClean itself, which is
active, and the `node_modules`, `dist` and `__pycache__` folders inside VS Code, pgAdmin and
Python installs are outside git, so the `git_ignored` guard keeps them.

## What changes in the knowledge packs (06)

- New clean case `python/pip-uninstall-leftovers` (auto, 7 days, not in use).
- New attribution areas: `wsl-docker/docker-data-custom` (Docker disks in a custom location,
  and copies left behind), `python/installations` widened to `?:\Python3?`/`?:\Python3??`,
  `python/pip-uninstall-leftovers`, `dev-tools/sdks` widened to `?:\LLVM`, and a new attribution-only pack
  `databases` (MongoDB server, PostgreSQL, pgAdmin).
- Code change needed: clean targets cannot start with `?:\` today (the engine drops them as
  "a target must be an absolute path"). Without it, the pip rule covers only the standard
  install folders and frees nothing on F:.

Owner decisions with exact actions: 07.
