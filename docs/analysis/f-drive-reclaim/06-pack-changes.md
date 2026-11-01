# 06 Proposed knowledge pack changes (and the code they need)

Nothing here is applied: the brief allows no edits to `data/packs/**` or code. Each change is
written as it would be committed. Every case is backed by a source; "observed" means observed
on the reference machine tonight.

| Change | Pack/case | Tier | GiB on F: | Needs code? |
|---|---|:-:|---:|---|
| new clean case | `python/pip-uninstall-leftovers` | auto | 3.79 (0 without the code change) | for F: only: `?:\` targets |
| new area | `python/pip-uninstall-leftovers` | — | 3.79 | no |
| widened area | `python/installations` (+ `?:\Python3?`, `?:\Python3??`) | — | 7.15 | no |
| new area | `wsl-docker/docker-data-custom` | — | 998.95 | no |
| widened area | `dev-tools/sdks` (+ `?:\LLVM`) | — | 3.86 | no |
| new pack (attribution only) | `databases`: `mongodb-server`, `postgresql`, `pgadmin` | — | 4.71 | no |

With these, the F: map explains ~1,019 GiB more (61 % of used instead of 0.03 %). The rest is
the owner's projects (dataset, repos), which no pack should name.

## 1. `python/pip-uninstall-leftovers` (new clean case, auto)

`data/packs/python/clean.toml`, after `uv-cache`:

```toml
# pip's uninstall stash left behind (sources: pip's src/pip/_internal/utils/temp_dir.py,
# AdjacentTempDirectory, and src/pip/_internal/req/req_uninstall.py, StashedUninstallPathSet;
# see pack.toml). Uninstalling or upgrading moves each top-level folder of the old version aside
# to a name that starts with '~' (torch -> ~orch, ~-rch), which no Python module or distribution
# may start with, and deletes it on commit. On Windows the delete fails when a DLL of the old
# version is loaded, and the stash stays; Python cannot import it and pip ignores it ("Ignoring
# invalid distribution ~orch"). Observed on reference machine: F:\Python313\Lib\site-packages\
# ~-rch (3.5 GiB of CUDA torch DLLs), ~orch, ~umpy and ~umpy.libs, left by a torch upgrade on
# 2026-06-25. A week-old stash belongs to no running pip. %ProgramFiles% installs are left to the
# deny-list.
[[rule]]
id       = "pip-uninstall-leftovers"
category = "dev-caches"
tier     = "auto"
scope    = "user"
strategy = "delete"
targets  = ['%LOCALAPPDATA%\Programs\Python\Python3*\Lib\site-packages\~*',
            '%APPDATA%\Python\Python3*\site-packages\~*',
            '%USERPROFILE%\miniconda3\Lib\site-packages\~*',
            '%USERPROFILE%\miniconda3\envs\*\Lib\site-packages\~*',
            '%USERPROFILE%\anaconda3\Lib\site-packages\~*',
            '%USERPROFILE%\anaconda3\envs\*\Lib\site-packages\~*']
require  = { min_age_days = 7, not_in_use = true }
```

After the `?:\` code change (below), add `'?:\Python3*\Lib\site-packages\~*'` to `targets`:
that is what reaches `F:\Python313`. Until then the case frees nothing on F:. A drive-letter
target (`'F:\Python3*\Lib\site-packages\~*'`) would work today but only on this machine; it
belongs in an owner pack (`%ProgramData%\MyClean\packs\python\`), not in the shipped one, and an
owner pack replaces the whole built-in python pack.

`data/packs/python/attribution.toml`, new area:

```toml
# pip's uninstall stash (pack.toml); the clean case deletes it after 7 days.
[[area]]
id       = "pip-uninstall-leftovers"
paths    = ['%LOCALAPPDATA%\Programs\Python\Python3*\Lib\site-packages\~*',
            '%APPDATA%\Python\Python3*\site-packages\~*', '?:\Python3*\Lib\site-packages\~*']
category = "dev-caches"
label    = "pip uninstall leftovers ('~' folders in site-packages)"
reclaim  = { kind = "clean-rule", ref = "pip-uninstall-leftovers", note = "Copies of uninstalled or upgraded packages that pip could not delete; Python never imports them." }
```

and the `installations` area gets the drive-root installs:

```toml
[[area]]
id       = "installations"
paths    = ['%LOCALAPPDATA%\Programs\Python', '%USERPROFILE%\miniconda3', '%USERPROFILE%\anaconda3',
            '%ProgramData%\miniconda3', '%ProgramData%\anaconda3', '?:\Python3?', '?:\Python3??']
category = "dev-toolchains"
label    = "Python and Conda installations"
reclaim  = { kind = "uninstall", note = "For Conda, `conda clean --all` removes cached packages." }
```

`data/packs/python/pack.toml`: `version = "1.2.0"`, and in `sources`:

```toml
  "https://github.com/pypa/pip/blob/main/src/pip/_internal/utils/temp_dir.py (AdjacentTempDirectory: a stash is named '~' plus characters that are not valid package names, e.g. torch -> ~orch, ~-rch) and https://github.com/pypa/pip/blob/main/src/pip/_internal/req/req_uninstall.py (StashedUninstallPathSet: the stash is deleted on commit, restored on rollback)",
  "observed on reference machine: F:\\Python313 and F:\\Python312 installed at the root of F:; F:\\Python313\\Lib\\site-packages\\~-rch (3.5 GiB), ~orch, ~umpy, ~umpy.libs left by a torch upgrade on 2026-06-25, next to torch 2.12.1+cpu",
```

Fixture `data/packs/python/tests/pip-leftovers.toml`:

```toml
# pip's uninstall stash in site-packages, as on the reference machine (F:\Python313: ~-rch,
# ~orch, ~umpy.libs next to torch 2.12.1+cpu). Installed packages and a stash a running pip may
# still roll back stay.

[[entry]]
path     = 'C:\Users\owner\AppData\Local\Programs\Python\Python313\Lib\site-packages\~-rch\lib\torch_cuda.dll'
size     = 966367641
age_days = 129
[[entry]]
path     = 'C:\Users\owner\AppData\Local\Programs\Python\Python313\Lib\site-packages\~orch\lib\torch_cpu.dll'
age_days = 129
[[entry]]
path     = 'C:\Users\owner\AppData\Local\Programs\Python\Python313\Lib\site-packages\~umpy.libs\libscipy_openblas64_.dll'
age_days = 361
[[entry]]
path     = 'C:\Users\owner\AppData\Local\Programs\Python\Python313\Lib\site-packages\~ydantic\main.py'
age_days = 0
[[entry]]
path     = 'C:\Users\owner\AppData\Local\Programs\Python\Python313\Lib\site-packages\torch\lib\torch_cpu.dll'
age_days = 129
[[entry]]
path     = 'C:\Users\owner\AppData\Local\Programs\Python\Python313\Lib\site-packages\torch-2.12.1+cpu.dist-info\METADATA'
age_days = 129
[[entry]]
path     = 'C:\Users\owner\AppData\Local\Programs\Python\Python313\Lib\site-packages\numpy\__init__.py'
[[entry]]
path     = 'C:\Users\owner\AppData\Roaming\Python\Python312\site-packages\~andas\core\frame.py'
age_days = 30

[[expect]]
kind     = "clean"
case     = "pip-uninstall-leftovers"
match    = ['C:\Users\owner\AppData\Local\Programs\Python\Python313\Lib\site-packages\~-rch',
            'C:\Users\owner\AppData\Local\Programs\Python\Python313\Lib\site-packages\~orch',
            'C:\Users\owner\AppData\Local\Programs\Python\Python313\Lib\site-packages\~umpy.libs',
            'C:\Users\owner\AppData\Roaming\Python\Python312\site-packages\~andas']
no_match = ['C:\Users\owner\AppData\Local\Programs\Python\Python313\Lib\site-packages\~ydantic',
            'C:\Users\owner\AppData\Local\Programs\Python\Python313\Lib\site-packages\torch',
            'C:\Users\owner\AppData\Local\Programs\Python\Python313\Lib\site-packages\torch-2.12.1+cpu.dist-info',
            'C:\Users\owner\AppData\Local\Programs\Python\Python313\Lib\site-packages\numpy',
            'C:\Users\owner\AppData\Local\Programs\Python\Python313\Lib\site-packages']

[[expect]]
kind     = "attribution"
case     = "pip-uninstall-leftovers"
match    = ['F:\Python313\Lib\site-packages\~-rch', 'F:\Python313\Lib\site-packages\~umpy.libs',
            'C:\Users\owner\AppData\Local\Programs\Python\Python313\Lib\site-packages\~orch']
no_match = ['F:\Python313\Lib\site-packages\torch', 'F:\Python313\Lib\site-packages']

[[expect]]
kind     = "attribution"
case     = "installations"
match    = ['F:\Python313', 'F:\Python312', 'C:\Users\owner\AppData\Local\Programs\Python']
no_match = ['F:\Python', 'F:\Python-tools', 'F:\MyClean']
```

## 2. `wsl-docker/docker-data-custom` (new area)

`data/packs/wsl-docker/attribution.toml`, after `docker-data`:

```toml
# Docker Desktop's disk in a custom "Disk image location" (Settings > Resources > Advanced;
# docs.docker.com, see pack.toml): CustomWslDistroDir in %APPDATA%\Docker\settings-store.json
# names a folder ...\DockerDesktopWSL holding disk\docker_data.vhdx and main\ext4.vhdx.
# Observed on reference machine: the live one in D:\Database\DockerDesktopWSL (1.55 TB), and a
# copy left behind by a move, renamed F:\WSL\wsl\DockerDesktopWSL.ANTIGO (998 GiB, last written
# 7 weeks before), which Docker never reads.
[[area]]
id       = "docker-data-custom"
paths    = ['?:\DockerDesktopWSL*', '?:\*\DockerDesktopWSL*', '?:\*\*\DockerDesktopWSL*',
            '?:\*\*\*\DockerDesktopWSL*']
category = "vm-disks"
label    = "Docker Desktop disk in a custom location, or a copy left by a move"
reclaim  = { kind = "manual-review", note = "Docker Desktop uses only the folder named by CustomWslDistroDir in %APPDATA%\\Docker\\settings-store.json. Any other DockerDesktopWSL folder (renamed, or left by a move) is a copy Docker never reads: check what you need is in the live one, then delete it. The live one moves with Settings > Resources > Advanced > Disk image location." }
```

`pack.toml`: `version = "1.5.0"`, and in `sources`:

```toml
  "observed on reference machine: CustomWslDistroDir = D:\\Database\\DockerDesktopWSL (settings-store.json), holding disk\\docker_data.vhdx (1.55 TB) and main\\ext4.vhdx, the docker-desktop Lxss BasePath; a timestamp-identical older copy renamed F:\\WSL\\wsl\\DockerDesktopWSL.ANTIGO (998 GiB, last written 2026-09-11) that nothing opens",
```

Fixture, appended to `data/packs/wsl-docker/tests/disks.toml`:

```toml
[[expect]]
kind     = "attribution"
case     = "docker-data-custom"
match    = ['D:\Database\DockerDesktopWSL', 'F:\WSL\wsl\DockerDesktopWSL.ANTIGO']
no_match = ['F:\WSL\wsl\main', 'F:\WSL\wsl', 'C:\Users\owner\AppData\Local\Docker\wsl']
```

`manual-review` keeps the bytes out of the map's "reclaimable" sum: a live disk must not be
counted as freeable. Telling the live folder from a copy is a code follow-up (below).

## 3. `dev-tools/sdks` (widened area)

```toml
[[area]]
id       = "sdks"
paths    = ['%ProgramFiles(x86)%\Windows Kits', '%ProgramFiles%\dotnet', '%ProgramFiles%\NVIDIA GPU Computing Toolkit',
            '%ProgramFiles%\LLVM', '%ProgramFiles%\Go', '%ProgramFiles%\Android', '?:\LLVM']
category = "dev-toolchains"
label    = "SDKs and toolchains (Windows SDK, .NET, CUDA, LLVM, Go, Android Studio)"
reclaim  = { kind = "uninstall", note = "Uninstall SDK versions you no longer build with." }
```

`pack.toml`: `version = "1.2.0"` (MINOR: a wider area, as the C: analysis bumped), source
addition `"observed on reference machine: LLVM installed at F:\\LLVM (3.9 GiB)"`. Fixture
(`tests/` of dev-tools, new expect): `match = ['F:\LLVM', 'C:\Program Files\LLVM']`,
`no_match = ['F:\LLVM\bin']`. `F:\Go` is left out: a folder named `Go` at a drive root is as
likely to be someone's projects.

## 4. New pack `databases` (attribution only)

`data/packs/databases/pack.toml`:

```toml
id          = "databases"
version     = "1.0.0"
title       = "Database servers"
description = "Where MongoDB and PostgreSQL servers and pgAdmin are installed, for review or uninstall. Never cleaned: their data folders hold the owner's databases."
sources     = [
  "https://www.mongodb.com/docs/manual/tutorial/install-mongodb-on-windows/ (default %ProgramFiles%\\MongoDB\\Server\\<version>, with bin, data and log)",
  "https://www.postgresql.org/download/windows/ (EDB installer: %ProgramFiles%\\PostgreSQL\\<major>, optionally with pgAdmin 4 inside)",
  "https://www.pgadmin.org/download/pgadmin-4-windows/ (standalone pgAdmin 4 installer)",
  "observed on reference machine: F:\\MongoDB\\Server\\8.0 (2.9 GiB: mongod.pdb and mongos.pdb 1.8, mongod.log 0.5, service stopped), F:\\PostgreSQL\\18 (1.0 GiB, with its own pgAdmin 4, service postgresql-x64-18) and a second, standalone F:\\pgAdmin 4 (pgAdmin 4 version 9.9)",
]
min_myclean = "0.1.0"

[applies_when]
any = [{ app_installed = '^(MongoDB \d|PostgreSQL \d|pgAdmin 4)' }, { process_image = "mongod.exe" },
       { process_image = "postgres.exe" }, { path_exists = '%ProgramFiles%\MongoDB\Server' },
       { path_exists = '%ProgramFiles%\PostgreSQL' }, { path_exists = '%ProgramFiles%\pgAdmin 4' }]
```

`data/packs/databases/attribution.toml`:

```toml
# Sources: pack.toml. The data folders inside hold the owner's databases: review or uninstall,
# never cleaned.

[[area]]
id       = "mongodb-server"
paths    = ['%ProgramFiles%\MongoDB\Server\*', '?:\MongoDB\Server\*']
category = "apps"
label    = "MongoDB server"
reclaim  = { kind = "uninstall", note = "bin\\*.pdb are debug symbols, only needed to read a crash dump; log\\mongod.log grows until rotated (logRotate). Export data before uninstalling." }

[[area]]
id       = "postgresql"
paths    = ['%ProgramFiles%\PostgreSQL\*', '?:\PostgreSQL\*']
category = "apps"
label    = "PostgreSQL server"
reclaim  = { kind = "uninstall", note = "The installer can add its own pgAdmin 4 here; one pgAdmin is enough. Dump the databases before uninstalling." }

[[area]]
id       = "pgadmin"
paths    = ['%ProgramFiles%\pgAdmin 4', '%LOCALAPPDATA%\Programs\pgAdmin 4', '?:\pgAdmin 4']
category = "apps"
label    = "pgAdmin 4 (standalone)"
reclaim  = { kind = "uninstall", note = "PostgreSQL's installer may already include pgAdmin 4." }
```

`data/packs/databases/tests/servers.toml`:

```toml
# The reference machine's servers, installed at the root of F:.

[[expect]]
kind     = "attribution"
case     = "mongodb-server"
match    = ['F:\MongoDB\Server\8.0', 'C:\Program Files\MongoDB\Server\7.0']
no_match = ['F:\MongoDB', 'C:\Users\owner\AppData\Local\MongoDBCompass']

[[expect]]
kind     = "attribution"
case     = "postgresql"
match    = ['F:\PostgreSQL\18', 'C:\Program Files\PostgreSQL\16']
no_match = ['F:\PostgreSQL']

[[expect]]
kind     = "attribution"
case     = "pgadmin"
match    = ['F:\pgAdmin 4', 'C:\Program Files\pgAdmin 4']
no_match = ['F:\PostgreSQL\18\pgAdmin 4']
```

`docs/packs.md`, shipped packs table, new row: `| databases | attribution (uninstall):
MongoDB and PostgreSQL servers, pgAdmin 4 |`; python row: add "pip's uninstall leftovers".

## What is deliberately not a pack case

| Finding | GiB | Why not |
|---|---:|---|
| Docker disk copy | 998.95 | A VM disk is never 100 % regenerable; telling a copy from the live disk needs code (below). |
| Election dataset (`urnas`, zips, read copy) | 281 | One person's project data. The old read copy has the project's own prune command (07). |
| MyClean build configurations | 85.4 | Covered by `cpp/cmake-artifacts` once the repo goes quiet; an "idle configuration in an active project" rule needs code and evidence (04). |
| MongoDB `.pdb` and log | 2.3 | Part of a server installed anywhere; deleting files of an installed product is the owner's call. |

## Code changes needed

| # | Change | Why | Where |
|---|---|---|---|
| 1 | **`?:\` in absolute clean targets** = every fixed local volume (as attribution already does). Today a `?:\…` target loads, then the engine drops it: `target_entries` requires `has_root_name()` and `resolve` lists `?:\`, which does not exist. | Reaches installs at drive roots (`F:\Python313`'s 3.79 GiB); attribution and relocation already accept `?:\`. Keep the deny-list as is: `volume_root`, `program_files` and `$` entries protect every volume. | `src/core/clean/engine.cpp` (`target_entries`), an `IFileTree`/probe for the fixed volumes, `src/core/packs/fixtures.cpp` (the fake machine's volumes), docs/packs.md |
| 2 | **Native commands get a working folder** (`%USERPROFILE%` of the token they run under) instead of inheriting the caller's. | `pnpm store prune` run from a folder on F: prunes `F:\.pnpm-store` (created at 00:46 tonight), not the real store; other tools resolve config from the working folder too. | `src/platform/cleanup/win32_command_runner.cpp:269` (`lpCurrentDirectory`) |
| 3 | (follow-up) A condition for "this Docker Desktop folder is not the live one": read `CustomWslDistroDir` (settings-store.json) and the Lxss BasePaths, then offer copies as an `ask` case. | 998 GiB on this machine; the case recurs whenever someone moves Docker's disk by copying. | a new probe in `src/platform/packs`, a condition kind in `src/core/packs` |
| 4 | (follow-up) A target condition "the folder contains `<name>`" (e.g. `CMakeCache.txt`), to reach idle CMake configurations inside active projects safely. | 56.8 GiB of idle configurations in `F:\MyClean\build`; needs weeks-old evidence before a threshold (04). | `src/core/rules/schema.hpp` (`Requirements`), `src/core/clean/engine.cpp` |

None of these was made: the brief allows no code edits.
