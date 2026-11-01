# 03 WSL (125 GiB on C:)

`wsl -l -v`: **Ubuntu-24.04** is WSL 1 (version 1, stopped). Its files are a `rootfs` folder:
`~\AppData\Local\Packages\CanonicalGroupLimited.Ubuntu24.04LTS_79rhkp1fndgsc\LocalState\rootfs`,
125.2 GiB. **docker-desktop** is WSL 2 and running; its disk is not on C: above 256 MiB. While the
Docker VM runs, its swap file (`%TEMP%\<GUID>\swap.vhdx`, 6.5 GiB) is in use.

## What the rootfs holds

| GiB | Linux path | What |
|---:|---|---|
| 93.9 | `/tmp` | 39,323 entries. Nearly all are `.tmpXXXXXX` folders: the Rust `tempfile` pattern, test fixtures of a graph-database project (`nodes.store`, `rels.store`, `adjacency.*.store`, LMDB `data.mdb`). By month: 2025-09 288, 2025-10 5,824, **2025-11 33,082**, 2025-12 116, and 10 since. A sample of 60 averages 2.6 MB. |
| 18.7 | `/usr` | `/usr/local` 8.2 (CUDA 13.0 4.9, Ollama 3.3), `/usr/lib` 4.5, `/usr/share` 4.4 (Swift 2.5), `/usr/bin` 0.8 |
| 9.0 | `/home/<user>` | `.rustup` 4.7, `.local` 1.9, `.npm` 0.65 (`_npx` 0.63) |
| 2.6 | `/opt` | NVIDIA Nsight Compute 1.2, Nsight Systems 1.0 |
| 1.0 | `/var` | apt lists 0.7 |

## Reclaim paths

1. **Prune `/tmp` from inside Linux (1), ~93 GiB.** WSL 1 never empties `/tmp`: there is no
   systemd-tmpfiles at start. Linux files may be changed only by Linux tools, never through the
   `rootfs` folder from Windows (learn.microsoft.com/windows/wsl/filesystems). The coordinator
   approved, and this task added, the native tool `wsl tmp prune` (src/core/cleanup) and the
   `auto` rule `wsl-docker/tmp-prune`. The tray runs, per distribution (Docker Desktop's are
   skipped):
   `wsl -d <name> -u root --exec find /tmp -mindepth 1 -maxdepth 1 -mtime +7 ! -name .*-unix -exec rm -rf {} +`.
   It measures `/tmp` with `du -sk` before and after. Open risk: on this `/tmp`, `du` and `rm`
   may exceed the generic 2-minute and 30-minute timeouts (15).
2. **Move the distribution to D: (2), 31.3 GiB, after the prune.** Recipe `wsl-docker/distro-files`
   does `wsl --export`, `wsl --import --version 1` on the target, a check, then `wsl --unregister`.
   Done after the prune, the archive and the copy are ~31 GiB instead of 125. Undo: the same
   recipe back.
3. **Inside the distribution (3).** `rustup toolchain list` / `uninstall` (4.7), the CUDA and
   Nsight versions in `/usr/local` and `/opt` (7.1), Ollama models (3.3), Swift (2.5) and
   `npm cache clean --force` (0.6). These are the owner's.

## Why not compress it

`scan-all` recommended compressing `%LOCALAPPDATA%\Packages`, estimating 104.5 GiB freed, and
almost all of that is this rootfs. That recommendation is wrong for three reasons:

- WOF compression from Windows rewrites the Linux files from Windows, which WSL does not support.
- `/tmp` should be deleted, not compressed. Its store files are zero-filled preallocations, so
  they compress well, and that is what inflated the estimate.
- A distribution that moves to D: leaves C: anyway.

The coordinator approved, and this task added, the exclusion `Exclusion::wsl_distribution` in
src/core/compress. The analyzer no longer counts a `LocalState\rootfs` or `Packages\<pkg>\rootfs`
as eligible, and CompressFolder never compresses inside one. `ext4.vhdx` was already excluded as
a disk image. The compression figure and the relocation figure are therefore never added
together.

## Pack changes (wsl-docker 1.4.0)

- New area `distro-tmp` (`…\rootfs\tmp`), pointing at the new rule `tmp-prune` (native, auto,
  user scope). The rule is offered only when a WSL 1 `rootfs\tmp` holds something: native targets
  are only measured. A WSL 2 `/tmp` lives inside `ext4.vhdx` and measures 0 from Windows.
- `docker-prune` changed category from package-managers to `containers`, and stays `ask`. It also
  deletes stopped containers, which can hold the owner's state.
