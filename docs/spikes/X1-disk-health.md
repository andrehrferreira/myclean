# X1 — Disk health without elevation (spike note)

**Status:** verified, non-elevated. Admin-only items are flagged and listed in §5 for an
elevated re-run. Run on 2026-10-31 on Windows 11 Pro 10.0.26200, not admin.
**Unblocks:** H1, H2. **Requirements touched:** HLT-1, HLT-2, HLT-3, HLT-4 and the
`disk-health` spec (Health Source per Drive, NVMe Health Log, ATA SMART for SATA Drives, USB
Bridge Fallback).

Serial numbers are redacted. Only query IOCTLs and ATA/NVMe **read** commands were sent; no
write, trim, self-test, SMART-enable or firmware command, and no handle with write access was
opened.

## 1. Questions

- **(a)** Which drives answer `IOCTL_STORAGE_QUERY_PROPERTY` for `StorageDeviceProperty`,
  `StorageAdapterProperty` and `StorageDeviceTemperatureProperty`?
- **(b)** Does `StorageDeviceProtocolSpecificProperty` return the NVMe SMART / Health log
  (page 02h) on all five internal NVMe drives?
- **(c)** Can the WD Red Plus SMART attributes be read (`SMART_RCV_DRIVE_DATA`,
  `IOCTL_ATA_PASS_THROUGH`)?
- **(d)** Is `MSFT_StorageReliabilityCounter` readable?
- **(e)** What needs admin: opening `\\.\PhysicalDriveN` with 0 access versus `GENERIC_READ`?
- **(f)** What does the ROG USB bridge allow?

## 2. Method

A throw-away probe (not in the repository) opened `\\.\PhysicalDrive0`–`11`, first with
access 0, then with `GENERIC_READ`, and sent on each handle: the three properties of (a), the
protocol-specific NVMe log query (device and adapter), `IOCTL_STORAGE_GET_COUNTERS`,
`IOCTL_STORAGE_PREDICT_FAILURE`, `SMART_GET_VERSION`, `SMART_RCV_DRIVE_DATA_EX`,
`SMART_RCV_DRIVE_DATA` and `IOCTL_ATA_PASS_THROUGH`, the last three carrying SMART READ DATA
(B0h/D0h). `Get-PhysicalDisk`, `Get-StorageReliabilityCounter` and `Get-PnpDevice` gave the
Windows view. The returned NVMe log pages and SMART sectors are the test fixtures in
`tests/unit/health/fixtures/` (they contain no serial numbers).

The access bits in the SDK headers (10.0.26100) explain the results:

| IOCTL | Access |
|---|---|
| `IOCTL_STORAGE_QUERY_PROPERTY`, `IOCTL_STORAGE_PREDICT_FAILURE`, `IOCTL_STORAGE_GET_COUNTERS` | `FILE_ANY_ACCESS` |
| `SMART_RCV_DRIVE_DATA_EX` (new in the Windows 10 `CO` SDK) | `FILE_ANY_ACCESS` |
| `SMART_GET_VERSION` | `FILE_READ_ACCESS` |
| `SMART_RCV_DRIVE_DATA`, `IOCTL_ATA_PASS_THROUGH`, `IOCTL_SCSI_PASS_THROUGH` | `FILE_READ_ACCESS \| FILE_WRITE_ACCESS` |

## 3. Results per drive

| Disk | Model (as Windows reports it) | Bus | Readable without admin | Values on 2026-10-31 |
|---|---|---|---|---|
| 0 | WDC WD101EFBX-68B0AN0 (WD Red Plus 10 TB) | SATA | device + adapter descriptor, temperature, `SMART_RCV_DRIVE_DATA_EX` (full attribute table), `IOCTL_STORAGE_PREDICT_FAILURE` | 40 °C, 26 868 h, reallocated 0, pending 0, uncorrectable 0, CRC 0, no failure predicted |
| 1 | WDC WD101EFBX-68B0AN0 (WD Red Plus 10 TB) | SATA | same as disk 0 | 41 °C, 26 643 h, all sector counters 0 |
| 2 | Samsung SSD 970 PRO 1TB | NVMe | descriptors, temperature (3 sensors), NVMe health log | 56 °C, 10 % used, spare 100 %, 0 media errors, 512 unsafe shutdowns, 233 TB written, 54 684 h |
| 3 | Samsung SSD 980 PRO 2TB | NVMe | same | 42 °C, 5 % used, **spare 95 %**, **498 media errors**, 276 unsafe shutdowns, 99 TB written, 3 656 h |
| 4 | WD_BLACK SN7100 4TB | NVMe | same | 41 °C, 0 % used, spare 100 %, 0 media errors, 50 TB written, 1 197 h |
| 5 | CT4000P3SSD8 (Crucial P3 4 TB) | NVMe | descriptors, NVMe health log; **temperature property fails** (Win32 1117) | 45 °C (from the log), 1 % used, spare 100 %, 0 media errors, 9.8 TB written, 17 132 h |
| 6 | Sabrent Rocket 4.0 2TB | NVMe | descriptors, temperature, NVMe health log | 42 °C, 6 % used, spare 100 %, 0 media errors, 135 TB written, 50 561 h |
| 7 | ROG ESD-S1C | USB (UAS, `USB\VID_0B05&PID_1932`) | descriptors only | nothing: see (f) |

Answers:

- **(a)** Every disk answers the device and adapter descriptors with a 0-access handle. The
  temperature property works on all except the Crucial P3 and the ROG (Win32 1117). The NVMe
  drives report their own warning and critical temperatures there (970 PRO 81/81 °C, 980 PRO
  82/85, SN7100 90/94, Sabrent 90/95); the WD Reds report a 65 °C warning.
- **(b)** Yes, on all five NVMe drives, device and adapter variants alike, with a 0-access
  handle. The 512-byte page decodes per the NVMe base specification (temperature in kelvin at
  byte 1, spare 3, threshold 4, % used 5, 128-bit counters from byte 32: data units written at
  48, power-on hours at 128, unsafe shutdowns at 144, media errors at 160).
- **(c)** Yes, without admin: `SMART_RCV_DRIVE_DATA_EX` returns the 512-byte SMART READ DATA
  sector (17 attributes, valid checksum), and `IOCTL_STORAGE_PREDICT_FAILURE` returns the same
  sector plus the drive's own failure prediction. `SMART_GET_VERSION`, `SMART_RCV_DRIVE_DATA` and
  `IOCTL_ATA_PASS_THROUGH` fail with access denied (5) because they need read or read/write
  access. Temperature is attribute 194 (low byte of the raw value; the upper bytes hold min/max).
- **(d)** `IOCTL_STORAGE_GET_COUNTERS` returns "invalid function" (1) on every disk, so the
  reliability counters are not reachable that way. `MSFT_StorageReliabilityCounter` over WMI
  (`root\Microsoft\Windows\Storage`) is denied without admin (`WBEM_E_ACCESS_DENIED`,
  0x80041003). The service runs as LocalSystem and can read it; not verified here.
- **(e)** Opening `\\.\PhysicalDriveN` with `GENERIC_READ` is denied (5) for every disk.
  Access 0 is enough for everything in (a)–(c). Only the pass-through IOCTLs need an elevated
  read/write handle.
- **(f)** The ROG ESD-S1C sits behind Windows' UAS driver. With a 0-access handle it rejects
  the NVMe log query and `SMART_RCV_DRIVE_DATA_EX` (invalid function) and the temperature
  property (1117). SAT pass-through (`IOCTL_SCSI_PASS_THROUGH` with ATA PASS-THROUGH(16)) needs a
  read/write handle and could not be tried. The enclosure takes an NVMe SSD, so a SAT command
  carrying an ATA SMART read will most likely be rejected by the bridge. Vendor NVMe tunnelling
  commands (ASMedia, JMicron and Realtek-specific CDBs) were not tried and are not used: they are
  outside the read-only command set allowed here.

## 4. Consequences for H1/H2

- The reader opens each disk with **access 0** and needs no elevation for the NVMe and SATA
  drives (7 of 8 here). The `platform_disk_health` smoke test reads all 7 non-elevated.
- SATA uses `SMART_RCV_DRIVE_DATA_EX` + `IOCTL_STORAGE_PREDICT_FAILURE`, not
  `SMART_RCV_DRIVE_DATA` or ATA pass-through.
- USB: NVMe log query and `SMART_RCV_DRIVE_DATA_EX` first (cheap, no rights), then SAT
  pass-through (needs a read/write handle), then the WMI reliability counters; otherwise the
  reading says "not readable over USB: …" with each reason.
- Identity is model + FNV-1a hash of the serial. Both WD Reds share a model, so the serial is
  needed; the disk number is not used.
- Not all NVMe drives answer the temperature property (Crucial P3), so the log's composite
  temperature is the primary value and the property only a fallback.
- **Finding for the owner:** the Samsung 980 PRO (disk 3, firmware 3B2QGXA7) already reports
  498 media and data-integrity errors and 95 % available spare after only 3 656 power-on hours.
  Its status is **warning** from the first read. According to public reports from 2023,
  firmware 3B2QGXA7 caused rapid health loss on 980 PRO drives and Samsung replaced it with
  5B2QGXA7 (not verified in this spike). Check for a firmware update in Samsung Magician and
  keep the data on G: backed up.

## 5. To verify in an elevated run (owner)

1. `Get-PhysicalDisk | Get-StorageReliabilityCounter` in an elevated PowerShell: which fields
   Windows fills for disk 7 (ROG). These are what the USB fallback will show.
2. The SAT path for the ROG: run
   `myclean_platform_disk_health_tests.exe --gtest_filter=Win32DiskHealthSource.*` elevated. The
   line for disk 7 shows either `source sat_pass_through` (the bridge forwards SAT) or the
   bridge's SCSI status in the "not readable over USB" reason, and whether the
   reliability-counter fallback returned values (`source reliability_counters`).
3. `ReliabilityCounters.ReadsOrSaysThatAdministratorRightsAreNeeded` elevated: it should print
   "readable (elevated run)".
