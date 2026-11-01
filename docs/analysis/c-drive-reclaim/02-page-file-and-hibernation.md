# 02 Page file and hibernation (170 GiB)

## Page file: 119.1 GiB, move it, don't shrink it

- `C:\pagefile.sys` is system-managed: `AutomaticManagedPagefile` is true, and C: is the only page
  file. It was 97.8 GB (91 GiB) allocated at the first look, 115 GiB in `scan-all` and 119 GiB in
  the map.
- **Commit charge 206.2 GB of a 224.4 GB limit, on 127.2 GB of RAM.** About 97 GB of committed
  memory is backed only by the page file. Usage inside the file is 14–18 GiB, but that is the
  wrong measure. Windows must be able to back every committed byte, so a 16–32 GiB page file
  would leave the machine at its commit limit and fail allocations. (This corrects the interim
  figure, after an independent review.)
- Crash dumps are set to small memory dump (`CrashDumpEnabled = 3`). That needs only a small page
  file on the boot volume, or a `DedicatedDumpFile`.

**Reversible action (2), restart required.** Make the page file on C: small: a fixed 4–8 GiB,
enough for the small dump. Add a system-managed page file on a healthy NVMe with room: D:
(4 TB NVMe, 991 GB free) or W: (4 TB NVMe, 1.76 TB free). Reclaim on C: is **~110 GiB**. The commit
limit stays the same or grows. MyClean's `page_file` setting (area `windows/page-file`) is the
action. Undo: set C: back to system-managed.

**Worth a look.** A committed load of ~206 GB is unusual even on 128 GB. The process that
commits it (a leaking build, a VM, or WSL/Docker memory) is worth finding. That is the
governor's subject, not this analysis's.

## Hibernation file: 50.9 GiB

- `C:\hiberfil.sys` is 54.6 GB. `HibernateEnabled = 1`, and the type is full (about 40 % of RAM).
- **Reduced** (`powercfg /h /type reduced`) keeps Fast Startup but not hibernate. It halves the
  file: **25.4 GiB** back. This is (2), reversible. It is what `scan-all` counted under remove.
- **Off** (`powercfg /h off`) removes the file: the other 25.4 GiB. It also ends hibernate and
  Fast Startup. This is (3), the owner's choice.

## swapfile.sys

16 MiB. Nothing to do.
