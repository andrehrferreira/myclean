# X7 — Whole-volume NTFS index (spike note)

**Status:** method chosen and implemented (S3, `src/core/index`, `src/platform/ntfs`); the
non-elevated numbers below are measured; the `$MFT` read itself needs an elevated run by the
owner (§5). Measured on 2026-10-31, Windows 11 Pro 10.0.26200, not admin, release build, at
below-normal CPU and background I/O priority, while other builds were running on the machine.
**Unblocks:** S3, the `ntfs-index` spec of phase 2b, `myclean map` / `scan-all` on whole volumes.

Only read requests were sent: `ReadFile` of `$MFT` clusters, `FSCTL_GET_NTFS_VOLUME_DATA`,
`FSCTL_GET_NTFS_FILE_RECORD`, `FSCTL_QUERY_USN_JOURNAL`, `FSCTL_READ_USN_JOURNAL`,
`FSCTL_ENUM_USN_DATA`, `FSCTL_READ_FILE_USN_DATA`. No journal was created or deleted.

## 1. Questions

- **(a)** What can be read without elevation?
- **(b)** How fast is scanner v1 (S1, `space::scan`) — the fallback — on real folders?
- **(c)** Which primitive gives names, parents and sizes on disk of a whole volume fastest:
  `FSCTL_ENUM_USN_DATA` plus a per-file record read, or the raw `$MFT`?
- **(d)** Can the in-memory index stay under 100 bytes per file?
- **(e)** How is the index kept current, and when must it be rebuilt?

## 2. Results without elevation

**(a) Access.** A throw-away probe (not in the repository) opened `\\.\C:` with access 0,
`FILE_READ_ATTRIBUTES` and `GENERIC_READ`:

| Request | access 0 / `FILE_READ_ATTRIBUTES` | `GENERIC_READ` |
|---|---|---|
| open `\\.\C:` | ok | **denied (5)** |
| `FSCTL_GET_NTFS_VOLUME_DATA`, `FSCTL_QUERY_USN_JOURNAL`, `FSCTL_ENUM_USN_DATA`, `FSCTL_READ_USN_JOURNAL`, `FSCTL_GET_NTFS_FILE_RECORD` | **invalid function (1)** | — |
| `FSCTL_READ_FILE_USN_DATA` on a file handle | ok | — |

So every volume-level request needs an elevated process (the service runs as LocalSystem).
`FSCTL_READ_FILE_USN_DATA` works for a single file without elevation; its output for
`C:\Windows\notepad.exe` is the recorded fixture of the USN parser
(`tests/unit/index/usn_test.cpp`), and `platform_ntfs` checks the parser against what Windows
returns for a fresh file on every run.

**(b) Scanner v1** (`X7.MeasuresScannerV1OnAFolder`; workers 1, then 4, then 8, so the first
pass is the cold one):

| Folder | Drive | Folders | Files | 1 worker (cold) | 4 workers | 8 workers |
|---|---|---|---|---|---|---|
| `C:\Windows\System32` | 970 PRO (NVMe) | 1 848 | 25 673 | 0.37 s, 73 k entries/s | 0.087 s, 315 k/s | 0.075 s, 369 k/s |
| `C:\Program Files` | 970 PRO (NVMe) | 21 984 | 156 766 | 7.9 s, 23 k/s | 0.85 s, 210 k/s | 0.62 s, 290 k/s |
| `K:\Assets` | WD Red Plus (HDD) | 5 712 | 109 030 | 38.4 s, **3.0 k/s** | 9.6 s, 12 k/s | 0.21 s, 542 k/s (cached) |
| `C:\` (whole volume) | 970 PRO (NVMe) | 586 582 | 3 523 936 | 221 s, 19 k/s | 51 s, 81 k/s | 41 s, 100 k/s |

Cold, a directory walk is bound by the seeks of the folder listings: about 20 k entries/s on
NVMe and 3 k entries/s on the HDD. A whole walk of C: (4.1 M entries, 276 folders denied without
admin) takes 41–221 s depending on parallelism and cache. At that rate the millions of files of the backup HDDs take
tens of minutes per volume. Warm (metadata cached), it reaches 0.3–0.5 M entries/s.

**CPU cost of an index build** (`X7.MeasuresTheIndexBuildFromASyntheticMft`, an in-memory
`$MFT` of 262 144 records, 202 000 in use): read, parsed and linked in 0.15 s on one thread,
**1.7 M records/s, 1.7 GB/s of `$MFT`**. The build is therefore bound by the read of the
`$MFT`, not by parsing.

**(d) Memory** (`VolumeIndexMemory.StaysBelow100BytesPerFile`): **71 bytes per name** for
names of about 20 characters — a 48-byte node, the UTF-8 name in one pool, a 4-byte slot per
`$MFT` record. Hard links are chained through the node (no hash map); a link's reparse tag is
kept in its unused size field. The build itself peaks higher (per-record facts, about 40 bytes
per record, freed by `finish()`).

## 3. Method chosen

**(c)** `FSCTL_ENUM_USN_DATA` returns one record per file with *one* name and no sizes. Sizes
would then need `FSCTL_GET_NTFS_FILE_RECORD` per file (one request per file, millions of
requests), and the other hard links of a file would still be missing. Reading the `$MFT`
directly gives everything in one sequential pass:

1. `FSCTL_GET_NTFS_VOLUME_DATA`: cluster and record size, `MftStartLcn`, `MftValidDataLength`.
2. Record 0 (`$MFT`) at `MftStartLcn`: the runs of its unnamed `$DATA` (and, for a very
   fragmented `$MFT`, the extension records its `$ATTRIBUTE_LIST` names, read with
   `FSCTL_GET_NTFS_FILE_RECORD`).
3. The runs in 4 MiB reads; each FILE record gets its update sequence restored and is parsed:
   `$STANDARD_INFORMATION` (attributes, times), every `$FILE_NAME` except DOS-only names (hard
   links with their parents), `$DATA` (logical size of the unnamed stream; allocation of every
   stream whose first part is in the record: the clusters actually allocated for compressed and
   sparse streams, so a Windows-compressed file counts its `WofCompressedData`),
   `$REPARSE_POINT` (tag). Extension records are folded into their base record.
4. Link the names into a tree (names whose parent is missing or unreachable are dropped and
   counted), pick the counted link of each hard-linked file (first in a depth-first walk by
   name, as scanner v1), add the totals.

`FSCTL_ENUM_USN_DATA` stays as a measurement and cross-check only (`enumerate_files`). The
`$MFT` is read with a page-aligned buffer because volume reads are non-cached.

**(e) Updates.** The journal position (`FSCTL_QUERY_USN_JOURNAL`) is noted *before* the
`$MFT` is read. A refresh reads `FSCTL_READ_USN_JOURNAL` from there; for each file only its
last entry matters: a deleted file is removed, any other is re-read with
`FSCTL_GET_NTFS_FILE_RECORD` and re-linked (its old contribution taken off every folder above,
the new one added); a folder keeps its node so its contents follow a move. The index is read
again from the `$MFT` when the journal was deleted or recreated (new id), wrapped past the
cursor, was overwritten while reading, the volume has no journal (the index never creates
one), or more than a quarter of the nodes are left over from deletions. Tests check that an
updated index equals one read again from the `$MFT`.

Folder growth (24 h / 7 d / 30 d) does not come from the journal — on C: it covers hours to
days — but from daily snapshots of the folders of at least 32 MiB in `index_folder_size`
(kept 32 days), compared with the current index.

## 4. Expected elevated numbers

From the measured parse rate and the drives' sequential read speed: about 1 s per GB of `$MFT`
on the NVMe drives and 5–7 s per GB on the WD Red Plus HDDs. C: holds about 4.1 M names, so
its `$MFT` is roughly 4–5 GB (1 KiB records): a few seconds to read, against 41–221 s for a
walk, and about 300 MB of index at 72 bytes per name. These are estimates until §5 is run.

## 5. What the owner runs elevated

From an **elevated** PowerShell (the release test needs no developer shell):

```powershell
$env:MYCLEAN_X7_VOLUME = 'C:'
$env:MYCLEAN_X7_FOLDER = 'C:\Windows\System32'
F:\MyClean\build\idx-rel\tests\unit\platform_ntfs\myclean_platform_ntfs_tests.exe --gtest_filter=X7.MeasuresTheMftIndexOfAVolume:Win32NtfsVolume.*
# Then one HDD:
$env:MYCLEAN_X7_VOLUME = 'K:'
$env:MYCLEAN_X7_FOLDER = 'K:\Assets'
F:\MyClean\build\idx-rel\tests\unit\platform_ntfs\myclean_platform_ntfs_tests.exe --gtest_filter=X7.MeasuresTheMftIndexOfAVolume
```

The test sets below-normal CPU and background I/O priority for itself, and prints: the `$MFT`
size, records, read-and-index time and rate, names, unparented names, bytes per name, size on
disk; the time of `FSCTL_ENUM_USN_DATA` over the same volume; one journal refresh; and, for
`MYCLEAN_X7_FOLDER`, how many folders have the same size on disk in the index and in a
scanner v1 scan of that folder (it expects at least 99 %; files written between the two reads
differ). Paste the `X7` lines into §4.

## 6. Risks

- **Placeholders.** The cloud-file bits (`RECALL_ON_DATA_ACCESS`, `RECALL_ON_OPEN`) are taken
  from `$STANDARD_INFORMATION`; if NTFS keeps them only in the directory entries, the index
  under-reports placeholders. The §5 run compares placeholder counts on a folder with OneDrive
  content when `MYCLEAN_X7_FOLDER` points there.
- **Named streams.** The index counts non-resident named streams; a directory listing does not
  (only WOF files, which scanner v1 corrects). A folder with large alternate data streams
  differs from scanner v1 by that amount.
- **Not counted by either:** directory index allocations (`$INDEX_ALLOCATION` of large
  folders). The index does count what a listing never shows: `$MFT`, `$LogFile`, `$Bitmap`,
  `$Secure:$SDS` and `$Extend\$UsnJrnl:$J` (part of the "not visible" bytes of `myclean map`).
- **Hard-link choice after moves.** The counted link of a hard-linked file is chosen when the
  file is read; a later move of a folder above it does not re-choose it until the file changes
  or the index is rebuilt. Folder totals then attribute the file to the other link's folder.
- **Volume serial.** File identities use the 64-bit serial of `FSCTL_GET_NTFS_VOLUME_DATA`;
  scanner v1 uses `FILE_ID_INFO`. Both should be the same 64-bit NTFS serial — the §5
  comparison confirms it (identities only matter when results of both are mixed).
