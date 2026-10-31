# Scan-All Specification

## ADDED Requirements

### Requirement: Machine-Wide Read-Only Scan
The system SHALL provide `myclean scan-all [--drives C:,D:,...] [--budget-minutes N] [--data <dir>] [--json]`, which runs in the CLI process without the service, in Windows background mode (PROCESS_MODE_BACKGROUND_BEGIN first, then BELOW_NORMAL_PRIORITY_CLASS), over every fixed volume with a drive letter or the ones listed. For each volume it MUST build the folder tree from the NTFS index when elevated and otherwise with scanner v1 stopped after N minutes (default 10), then run the disk map, the cleaning rules and developer artifacts, the compression analyzer on the largest program, game and toolchain folders and the packs' compression targets, the drive maintenance analysis, the cold-data finder, the placement advisor and the cross-drive duplicate finder. It MUST NOT delete, move, compress, defragment or change anything, follow a link or open a cloud file; it reports what each finding's action would do. Progress SHALL go to standard error. A drive whose health is not good MUST still be scanned and reported.

#### Scenario: Partial tree on a large HDD
Given K: holds several TB in more files than scanner v1 lists in 8 minutes
When `myclean scan-all --budget-minutes 8` runs
Then K:'s tree stops after 8 minutes and K: is marked partial with a note
And its cold data is not judged while its other findings cover what was listed

#### Scenario: Bad arguments
Given the command line `myclean scan-all --budget-minutes 0`
When it runs
Then it exits with the usage exit code and prints the scan-all usage, scanning nothing

### Requirement: Cold Data
The system SHALL propose for archiving every outermost folder of at least 1 GiB on an NVMe or SATA SSD whose newest file creation or last write, over its whole subtree, is older than 6 months, as an archive finding with a move_folder plan to `<HDD>\Archive\<letter>\<path>` on the healthy HDD with the most free space that keeps 10 % free afterwards. It MUST NOT propose a folder under Windows, Program Files, ProgramData or another system folder of a volume root, a folder that holds a running process's current or image folder, or a folder whose subtree was not completely listed. Without a qualifying HDD the finding SHALL be manual-review. Last-access time MUST NOT be used.

#### Scenario: Old folder on NVMe
Given C:\Data\Old (3 GiB, last written 2025-01-10) and C:\Data\New (written last week) on an NVMe drive
And K: is a healthy HDD with room
When cold data is found on 2026-10-31
Then C:\Data\Old is an archive finding with target K:\Archive\C\Data\Old
And neither C:\Data nor C:\Data\New is proposed

#### Scenario: Program Files and folders in use stay
Given C:\Program Files\App and C:\Data\InUse are 2 GiB, untouched for two years, and a process runs from C:\Data\InUse
When cold data is found
Then neither folder is proposed

### Requirement: Placement Advice
The system SHALL give relocate findings: on a drive whose health is warning or critical, every root folder of at least 1 GiB that is not a system folder, urgent, to the best healthy internal drive with room; an active project (a folder holding `.git`, changed within 30 days) on a QLC NVMe or USB drive, outermost only, to a healthy NVMe or SATA SSD; a game location of the attribution on a USB drive, to a healthy NVMe or SATA SSD; and on the system drive every attribution location whose reclaim kind is relocate, with a relocate_cache plan naming the cache and its target folder. A finding without a qualifying target SHALL be manual-review.

#### Scenario: Failing drive first
Given K: reports a pending sector and holds K:\Media (2 GiB)
When placement is advised
Then K:\Media is an urgent relocate finding to D:\Media and no other placement advice is given for K:

#### Scenario: Project on QLC
Given Q:\src\app holds `.git` and was changed 6 days ago on a QLC NVMe drive
When placement is advised
Then Q:\src\app is a relocate finding with target D:\src\app on the NVMe drive D:

### Requirement: Cross-Drive Duplicates
The system SHALL find identical files of at least 1 MiB across all trees with space::find_duplicates (size, then a hash of the first 64 KiB, then of the whole content; links, cloud placeholders and further hard links skipped), largest sizes first, reading at most 8 GiB of content and stopping at the time budget. A hash that does not fit what is left of the byte budget MUST be skipped without stopping the search, which SHALL stop only when not even a 64 KiB head hash fits. In each set it SHALL keep one copy, chosen on a healthy drive, then inside a library or workspace root, then outside Downloads, TEMP and the Recycle Bin, then the newest, then the first by path, and give one deduplicate finding per drive of the other copies with a quarantine_items plan whose `keep` names the kept copy.

#### Scenario: Keep the library copy
Given the same photo in C:\Users\u\Downloads, D:\Docs and on the failing drive K:
When duplicates are found with D:\Docs as a library
Then D:\Docs\p.jpg is kept and C: and K: each get a deduplicate finding

#### Scenario: Hash budget
Given two sets of 100-byte and 10-byte files and a budget of 215 bytes
When duplicates are found
Then 210 bytes are hashed, one hash is reported as skipped for the budget and only the 100-byte set is found

#### Scenario: A pair larger than the budget
Given two identical 300 KiB files, two identical 10 KiB files and a budget of 200 KiB
When duplicates are found
Then the full hashes of the 300 KiB files are skipped and the 10 KiB pair is still found

### Requirement: Unified Report
The system SHALL produce one report with the drives (size, used, free, health, class, explained share of used space, what the tree could not see, notes and the counted bytes per action), the counted bytes per action for the machine, and every finding grouped by action (remove, deduplicate, relocate, archive, compress, defrag-trim, uninstall-review, manual-review), each with its drive, path, bytes, counted bytes, confidence, reason and plan (an action kind and its parameters), as text or as one JSON document with `--json`. Map places become findings by their reclaim kind: a native or handler rule's place is a remove finding with the rule's command or handler, a delete or quarantine rule's place is left to the rule engine's candidates, hibernation is half its size under remove, the page file above 4 GiB is a relocate finding, uninstall is uninstall-review, manual-review and unexplained folders are manual-review. On a drive whose health is not good, a finding that would write there (remove, deduplicate, compress, defrag-trim) MUST become manual-review, its reason naming the health.

#### Scenario: Retrim due
Given D: was last retrimmed 40 days ago
When the report is built
Then D: has a defrag-trim finding with plan retrim_volume volume=D:\

#### Scenario: Writes held on a failing drive
Given a remove finding on K:, whose health is warning
When the report is built
Then the finding is manual-review and its reason starts with "held: the drive's health is warning"

### Requirement: No Double Counting
The system MUST count each byte of a drive's tree under one finding only: the one whose folders or files cover it with the first action in the order remove > deduplicate > relocate > archive > compress > defrag-trim > uninstall-review > manual-review, and within one action the most precise one (deepest path; the earlier finding on a tie). A finding SHALL count its bytes times the share of its footprint it owns, at most all of them; a finding with no folder or file in the tree counts all its bytes. Totals per drive and for the machine SHALL be sums of counted bytes.

#### Scenario: Remove before compress
Given C:\Tools (100 GiB) estimated to compress by 30 GiB, holding C:\Tools\Cache (10 GiB) proposed for removal
When the report is built
Then the removal counts 10 GiB and the compression counts 27 GiB

#### Scenario: Duplicate copy inside an archive candidate
Given C:\Media (21 GiB) is an archive candidate and holds a 1 GiB duplicate copy proposed for deduplication
When the report is built
Then the deduplication counts 1 GiB and the archive counts 20 GiB
