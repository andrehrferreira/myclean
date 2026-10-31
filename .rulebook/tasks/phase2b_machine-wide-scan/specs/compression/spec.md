# Compression Specification

## ADDED Requirements

### Requirement: Sampled Compression Estimate
The system SHALL estimate, for a folder of a scan, the bytes that WOF compression would free with each of XPRESS4K, XPRESS8K, XPRESS16K and LZX, and with NTFS LZNT1 for information only, by compressing samples of file content in memory, and MUST report each estimate with a 95 % interval and a confidence between 0 and 1. Samples SHALL be windows of at most 64 KiB at equally spaced positions over the eligible files laid end to end, visited in bit-reversed order, so that each file is sampled in proportion to its size and a stopped run still covers the whole folder. The content read SHALL stay within a byte budget (default 64 MiB) and a time budget (default 2 minutes), every read MUST run at background CPU and I/O priority, and the estimate MUST NOT write, follow a link or open a cloud file. XPRESS sizes SHALL be measured with XPRESS Huffman per 4, 8 and 16 KiB chunk, the format WOF writes; LZX, which has no public compressor, SHALL be modelled as 1.12 times the savings of XPRESS Huffman on 32 KiB chunks, with a wider interval and lower confidence.

#### Scenario: Estimate matches what WOF saves
Given a folder of six program-like files and one file of random bytes, all last written 60 days ago
When the folder is estimated with a budget that covers all of it and then compressed with XPRESS4K
Then the estimated XPRESS4K savings are within 10 % of the measured drop in size on disk

#### Scenario: Budget bounds the reads
Given a folder of 100 files of 1 MiB and a byte budget of 8 MiB
When the folder is estimated
Then 128 windows of 64 KiB are read and no more than 8 MiB in total

#### Scenario: Samples proportional to size
Given ten files of 1 MiB and one file of 90 MiB with a budget of 100 windows
When the folder is estimated
Then 90 windows are read from the large file and one from each small file

#### Scenario: Time budget keeps the spread
Given 64 files of 1 MiB, one window each, and a time budget that allows eight samples
When the folder is estimated
Then eight samples are read, spread over files 0, 32, 16, 48, 8, 40, 24 and 56

#### Scenario: Uniform samples give full confidence
Given every sample compresses to the same ratio
When the folder is estimated
Then each XPRESS estimate has confidence 1 and an interval equal to the estimate
And the LZX estimate has confidence 0.95 and a wider interval

### Requirement: Compression Exclusions
The system MUST leave uncompressed, in both the estimate and the action: reparse points, cloud placeholders, further hard links of a counted file, files not larger than one cluster, files already smaller on disk than their size (WOF, NTFS-compressed or sparse), compressed formats by extension (pictures, audio, video, archives and packages), virtual disks (VHD, VHDX, AVHD(X), VMDK, VDI, QCOW2), databases, their journals and logs (SQLite, MDF/LDF, EDB, `*.db`, `*.wal`, `*.log` and similar), `*.sys` files, files modified within the recent window (default 30 days), everything under `%WINDIR%` (CompactOS covers it), and extensions whose samples kept more than 90 % of their size under XPRESS16K (measured incompressible, such as Oodle-packed game archives). Every exclusion SHALL be reported with its file count and size on disk.

#### Scenario: Excluded files are never read
Given a folder holding a link, a cloud file, a hard link, a 4 KiB file, an NTFS-compressed DLL, a JPEG, a VHDX, a SQLite database, a log, a driver, a file written 3 days ago, a subfolder under the Windows folder and one plain DLL
When the folder is estimated
Then only the plain DLL is sampled and every other file is counted under its exclusion reason

#### Scenario: Incompressible extension measured
Given a game folder whose `.pak` files do not compress
When the folder is estimated
Then `.pak` is reported as measured incompressible and those files are left out of the estimate

#### Scenario: Excluded files untouched by compression
Given a sandbox folder with a JPEG, a VHDX, a database, a small file, a recently written file, an NTFS-compressed file and a measured-incompressible file
When CompressFolder compresses the folder
Then none of those files is WOF-compressed, and their content and last-write times are unchanged

### Requirement: Algorithm Recommendation
The system SHALL recommend LZX for toolchain, engine and other Program Files installs, XPRESS8K for installed games (stating the LZX estimate as the alternative) and XPRESS4K for other folders, recognizing the category from path markers (`.rustup`, Visual Studio, Windows Kits and other SDKs; Unreal Engine, Unity and Godot editors; Steam, Epic, GOG, Xbox and other launcher libraries; Program Files). It MUST recommend nothing for a folder where more than 25 % of the bytes changed within the recent window, nor when the recommended algorithm would save less than 64 MiB or less than 10 % of the eligible bytes.

#### Scenario: Toolchain gets LZX
Given `C:\Users\owner\.rustup\toolchains` with 200 MiB of compressible files
When the folder is estimated
Then the category is toolchain and the recommendation is LZX

#### Scenario: Game gets XPRESS8K
Given a folder under `steamapps\common` with 200 MiB of compressible files
When the folder is estimated
Then the category is game and the recommendation is XPRESS8K with the LZX savings named in the reason

#### Scenario: Frequently written folder
Given a folder where half of the bytes were modified yesterday
When the folder is estimated
Then nothing is recommended and the reason says it is frequently written

### Requirement: Reversible Folder Compression
The system SHALL provide the action `CompressFolder` (tier ask, never automatic, run by the service) that previews one item per file the exclusion rules allow, with its identity and size on disk, and applies WOF compression (`WofSetFileDataLocation` with `WOF_FILE_COMPRESSION_INFO`) file by file at background priority. Each file MUST be opened denying other writers and compressed only while it is still the previewed file, a plain uncompressed file and not open for writing elsewhere; otherwise it is skipped and counted. The action MUST keep file content and last-write times unchanged, SHALL stop at a byte budget, a time budget or a cancel, and SHALL resume on a later run by skipping files already WOF-compressed. Verify SHALL re-measure the size on disk of the compressed files; undo SHALL remove the WOF backing of each compressed file (`FSCTL_DELETE_EXTERNAL_BACKING`), restoring every byte, and MUST go on through every file before reporting a failure. LZNT1 MUST never be applied.

#### Scenario: Compress and undo restore content exactly
Given six compressible files in a sandbox folder
When CompressFolder applies XPRESS4K, verifies and undoes
Then verify reports a smaller size on disk as better
And after undo no file is WOF-compressed, each takes at least its size on disk, and every byte equals the original

#### Scenario: Each WOF algorithm round trip
Given a 1 MiB compressible file
When it is compressed with XPRESS4K, XPRESS8K, XPRESS16K and LZX in turn, each followed by decompression
Then each compression reports that algorithm and a smaller size on disk, the content reads unchanged, and the last-write time never changes

#### Scenario: File in use is skipped
Given another program holds the file open for writing
When CompressFolder reaches that file
Then the file is not compressed and is counted as in use

#### Scenario: Replaced file is skipped
Given a previewed file was replaced by another file at the same path
When CompressFolder applies the preview
Then the new file is never handed to WOF and is counted as changed

#### Scenario: Resume after a budget
Given a folder of four 1 MiB files and a byte budget of 2 MiB
When CompressFolder applies the preview twice, the second time without a budget
Then the first run compresses two files and reports that it stopped
And the second run counts two files as already compressed and compresses the other two

#### Scenario: Incompressible file left as it was
Given a file of random bytes
When WOF is asked to compress it with LZX
Then the file is reported as not beneficial and stays uncompressed with unchanged content

### Requirement: CompactOS
The system SHALL provide the action `CompactOs` (tier ask, never automatic, run by the service) that reads whether Windows' own files are WOF-compressed from a sample of System32 libraries, independent of the display language, previews one item with the estimated savings when CompactOS is off, applies with `compact /compactos:always` and undoes with `compact /compactos:never`. The outcome MUST be judged by the state read afterwards, never by compact's localized text. Tests MUST NOT run either command.

#### Scenario: Already on
Given CompactOS is on
When CompactOs is previewed and applied
Then the preview is empty and no command runs

#### Scenario: Turned on and off again
Given CompactOS is off and compact turns it on
When CompactOs is applied, verified and undone
Then `compact /compactos:always` runs, verify reports better, and undo runs `compact /compactos:never`

#### Scenario: Command without effect
Given CompactOS is off and compact exits with code 5 leaving it off
When CompactOs is applied
Then the action fails naming exit code 5 and compact's first output line
