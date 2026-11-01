# 01 Method and accounting

## Sources (all read-only)

| When (local) | What | Notes |
|---|---|---|
| 08:22 | `myclean map F:\ --min-gb 0.25 --depth 9 --json` (given) | Not elevated. Scanned 1,662.9 GiB in 2,068,128 folders and 7.8 M entries; not visible 15.5; explained 0.5 (Recycle Bin only); 3 unreadable folders (`Config.Msi`, `System Volume Information`, the SYSTEM Recycle Bin). |
| 08:22 | the map's stderr | `pack 'wsl-docker': … rule 'tmp-prune': field 'command' is not a native command MyClean runs: 'wsl tmp prune'`: the binary that ran the map is older than the data, so the wsl-docker areas were not loaded. They would not have explained the F: disk anyway (02). |
| 08:30–09:30 | targeted reads | PowerShell at below-normal priority: `Get-ChildItem` sizes, file times and attributes; the Lxss keys; Docker's `settings-store.json` and `settings.json`; an exclusive-open test on the vhdx (succeeded, closed at once: nobody had it open); `fsutil sparse queryflag`; services; Uninstall keys; PEP 514 keys; `git log`/`check-ignore`/`rev-parse`; the election project's README, `.gitignore`, `src/config.js`, `src/etapas.js`, `src/banco-leitura.js`, `data/db/leitura/atual.json`. |
| 09:00 | project-marker walk | A Python walk of F:\ as `scan_artifacts` does it (depth 8, the `skip` names of `data/workspaces.toml`), with `F:\WSL` and the dataset's `raw` and `parsed` left out. 440 marker folders. |
| 09:10 | compressibility samples | The first 48 MB of eight files compressed in memory with Deflate in 32 KiB chunks rounded up to 4 KiB clusters, close to how WOF LZX works. A rough guide, not a measurement of LZX itself. |

Nothing was deleted, moved, compressed or changed. Two deviations: one
`F:\Python313\python.exe -c "import torch"` to read the installed version (Python may have
refreshed `.pyc` files), and one `fsutil fsinfo ntfsinfo F:` that was refused (needs admin).

## Accounting to 100 %

The volume is 2,000,381,014,016 bytes (1,863.0 GiB); 1,802,189,852,672 used (1,678.4 GiB);
184.6 GiB free.

| GiB | Part |
|---:|---|
| 1,662.9 | seen by the scanner (file sizes of the tree) |
| 15.5 | not visible: used bytes minus scanned bytes |
| **1,678.4** | used |

Within the 1,662.9 GiB, the top-level folders add up exactly:

| GiB | Folder | File |
|---:|---|---|
| 998.95 | `F:\WSL` | 02 |
| 560.97 | `F:\Node` (one project: `auditoria-2turno-eleicao2026`) | 03 |
| 76.72 | `F:\MyClean` (76.71 of it `build`) | 04 |
| 8.69 | `F:\MyClean-deploy` (8.57 `build`) | 04 |
| 7.01 | `F:\Python313` | 05 |
| 3.86 | `F:\LLVM` | 05 |
| 2.94 | `F:\MongoDB` | 05 |
| 1.09 | `F:\IDEs` (VS Code) | 05 |
| 1.00 | `F:\PostgreSQL` | 05 |
| 0.77 | `F:\pgAdmin 4` | 05 |
| 0.50 | `F:\$RECYCLE.BIN` | 05 |
| 0.41 | Go 0.20, Python312 0.14, OndeSaiMaisBarato 0.05, Protoc 0.01, Falcor, CC4, `.pnpm-store`, `desktop.ini` | 05 |
| **1,662.91** | | |

Each file breaks its folder down to folders of 0.25 GiB or files, so more than 99.9 % of the
used bytes are named.

## The "deep tree of small folders"

The map's progress shows it: 64,000 folders held 1,432 GiB, then the count rose to 2,068,128
while the size grew only 230 GiB. That is `data\raw\{1t,2t}\urnas\<uf>\<municipality>\<zone>\<section>\<hash>\`
of the election project: one folder per voting section and one per signed package, with six
files each (`aux.json` ~0.5 KB, `bu.dat` ~1 KB, `rdv.dat` ~3 KB, `imgbu.dat` ~6 KB, `vota.vsc`
~60 KB, `log.jez` ~200 KB in the sample `sp\71072\0001\0001`). The README counts 499,207
packages per round, so ~1 M sections, ~2 M folders and ~6 M files. It is the zips extracted by
the project's own collection step (03).

## The 15.5 GiB not visible

The non-elevated scanner sums file sizes. NTFS also spends space it does not see:

- **cluster slack**: every non-resident file takes whole 4 KiB clusters. In the sample
  section the six files waste ~10 KiB; over ~1 M sections that is **~10 GiB**;
- **MFT records**: one per file or folder, ~7.8 M entries; at 1 KiB per record **~7 GiB**
  (the record size needs admin to read);
- System Volume Information (shadow copies, last written 2026-09-30) and `Config.Msi`: not
  readable without admin; `vssadmin list shadowstorage /for=F:` (elevated) gives the figure.

Slack plus MFT already exceed 15.5, so the shadow storage on F: is probably small or zero. The
slack goes with the `urnas` tree; the MFT does not shrink, but its records are reused.

## Why the map explained only 0.5 GiB

Every area of the packs names a place under the profile, Program Files or ProgramData, or a
`?:\` system file. F: holds custom folders: a Docker disk moved by hand, a dataset, toolchains
installed at the drive root. Section 06 adds areas for the ones that are general knowledge
(Docker's custom disk location, Python, LLVM, MongoDB, PostgreSQL, pgAdmin). The dataset and
the repos stay unexplained by design: they are the owner's own projects.
