# 02 The Docker Desktop disk copy (`F:\WSL`, 998.95 GiB)

| GiB | File | Created | Last written |
|---:|---|---|---|
| 998.40 | `F:\WSL\wsl\DockerDesktopWSL.ANTIGO\disk\docker_data.vhdx` (1,072,010,625,024 bytes, not sparse) | 2024-08-12 17:04 | 2026-09-11 07:50 |
| 0.09 | `F:\WSL\wsl\DockerDesktopWSL.ANTIGO\main\ext4.vhdx` | 2026-08-30 | 2026-09-11 07:50 |
| 0.47 | `F:\WSL\wsl\main\ext4.vhdx` | 2024-08-12 17:04 | 2025-10-17 13:56 |
| 0 | `F:\WSL\wsl\DockerDesktopWSL.ANTIGO\distro` (empty) | 2023-07-17 | |

## It is not in use

- Docker Desktop's current setting, `%APPDATA%\Docker\settings-store.json`, has
  `"CustomWslDistroDir": "D:\\Database\\DockerDesktopWSL"`. Only the legacy `settings.json`
  still names `F:\WSL\wsl\DockerDesktopWSL`, a folder that no longer exists (the F: one was
  renamed `.ANTIGO`, Portuguese for "old").
- The only WSL registration (HKCU Lxss) is `docker-desktop`, BasePath
  `\\?\D:\Database\DockerDesktopWSL\main`, Flags 15. Nothing points at F:.
- `D:\Database\DockerDesktopWSL\disk\docker_data.vhdx` is 1,552,857,169,920 bytes and was
  written at 08:38 today; Docker Desktop and `vmmemWSL` have run since 2026-10-24.
- The F: disk opened exclusively at 09:00 (the test closed it at once): no process holds it.
- Both folders have identical creation times (`disk` 2025-10-17 13:57:34, the vhdx
  2024-08-12 17:04:54): the D: folder is a timestamp-preserving copy of the F: one, made after
  2026-09-11, when the F: disk was last written. The D: disk has grown 447 GiB since.

So the F: disk is a 7-week-old snapshot of Docker's images, containers and volumes. Docker never
reads it again. Whether anything in it is worth keeping (a database volume of that date, say) is
the owner's call: status 3.

## The two small `ext4.vhdx`

`main\ext4.vhdx` is the disk of the `docker-desktop` system distribution, which Docker Desktop
recreates (D:'s `main` was created 2026-10-24 23:51, when Docker started). Neither F: copy is
registered. They go with the folder.

## Action

1. Start Docker Desktop and check the containers and volumes you need are there (they are on D:).
2. Delete `F:\WSL` (998.95 GiB). It is too large for the Recycle Bin: Shift+Delete, or
   `Remove-Item -LiteralPath F:\WSL -Recurse` from a terminal.
3. Optional: in Docker Desktop, Settings > Resources > Advanced, re-save the disk location so
   the legacy `settings.json` stops naming F:.

Freed: 998.95 GiB; F: goes from 184.6 to ~1,183.5 GiB free (63.5 %).

## Pack knowledge

The wsl-docker pack knows only Docker's default location (`%LOCALAPPDATA%\Docker\wsl`). A disk
in a custom location (D:'s, 1.55 TB) and a copy left behind (F:'s) are unexplained in the map.
06 adds an attribution area `docker-data-custom` for both. Telling the current folder from a
copy needs `CustomWslDistroDir`, which no pack field can read: 06 lists that as a code
follow-up. No clean case: a VM disk is never 100 % regenerable.

Out of scope here but worth the coordinator's eye: the live D: disk is 1.55 TB. Docker's own
`docker system prune` (`wsl-docker/docker-prune`, ask) and compacting the vhdx afterwards
(Docker Desktop > Troubleshoot > Clean / Purge data, or `Optimize-VHD`) are the ways to shrink
it.
