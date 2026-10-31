# Disk Health Specification

## ADDED Requirements

### Requirement: Health Source per Drive
The service SHALL determine for every physical disk discovered at runtime how its health can be read (NVMe health log, ATA SMART, USB pass-through, or reliability counters only) and SHALL report that source with every health reading.

#### Scenario: Every drive has a source
Given the reference machine with 8 physical disks
When the first health read completes
Then each of the 8 disks has a health reading or an explicit statement of why it is not readable, together with the source used

### Requirement: NVMe Health Log
The service SHALL read, for every NVMe drive, the SMART/Health log and temperature through `IOCTL_STORAGE_QUERY_PROPERTY`: temperature, percentage used (wear), available spare, media and data integrity errors, unsafe shutdowns and data units written.

#### Scenario: Internal NVMe drive
Given the Samsung 980 PRO is attached over NVMe
When its health is read
Then the reading contains temperature, percentage used, available spare, media errors, unsafe shutdowns and data written

### Requirement: ATA SMART for SATA Drives
The service SHALL read ATA SMART attributes from every SATA drive: reallocated sectors, pending sectors, uncorrectable sectors, temperature, power-on hours and interface CRC errors.

#### Scenario: WD Red Plus
Given a WD Red Plus 10 TB on SATA
When its health is read
Then the reading contains reallocated, pending and uncorrectable sector counts, temperature, power-on hours and CRC error count

### Requirement: USB Bridge Fallback
For a drive behind a USB bridge the service SHALL try SAT or vendor pass-through first, and when the bridge blocks it SHALL state "not readable over USB" and fall back to the Windows storage reliability counters.

#### Scenario: Bridge blocks pass-through
Given the ROG ESD-S1C on USB rejects SMART and NVMe pass-through
When its health is read
Then its reading says "not readable over USB", carries the reliability-counter values that Windows provides, and the source is reliability counters

#### Scenario: Bridge allows pass-through
Given a USB bridge that forwards SAT commands
When the drive's health is read
Then the full SMART reading is returned and no "not readable over USB" statement is shown

### Requirement: Read Cadence and History
The service SHALL read the health of every drive every 10 min, SHALL store one health snapshot per drive per day in `health_sample` for 2 years, and SHALL raise any resulting alert within 1 h of the change appearing on the drive.

#### Scenario: Daily snapshot
Given the service has run for 3 days
When the health history of disk 0 is queried
Then it contains one snapshot per day

#### Scenario: Missed day
Given the PC was off for a whole day
When the service starts the next day
Then a health snapshot is taken at that start, and no snapshot is invented for the missed day

### Requirement: Delta Alerts
The service MUST raise a disk-health alert whenever a drive reports any new media error, reallocated sector or pending sector compared with its previous reading, even when every threshold is still met.

#### Scenario: First media error
Given an NVMe drive had 0 media errors at its previous reading
When it reports 1 media error
Then a disk-health alert names the drive, the counter, the old value 0 and the new value 1

#### Scenario: New pending sector
Given a WD Red had 0 pending sectors yesterday
When it reports 2 pending sectors
Then a disk-health alert is raised although no threshold is exceeded

#### Scenario: Unchanged counter
Given a drive reported 3 reallocated sectors at its previous reading
When it again reports 3 reallocated sectors
Then no new delta alert is raised

### Requirement: Temperature Thresholds
The service SHALL raise a temperature alert when a drive exceeds the limit defined for its model in `data/drive-specs.json`.

#### Scenario: Above model limit
Given drive-specs.json sets 70 °C for the drive's model
When the drive reports 74 °C
Then a temperature alert names the drive, its temperature and the 70 °C limit

#### Scenario: Unknown model
Given a drive whose model is not in drive-specs.json
When its temperature is read
Then the temperature is shown, it is stated that no rated limit is known, and no temperature alert is raised

### Requirement: Endurance Projection
The service SHALL compute, for each SSD whose rated TBW is known, the TBW used versus rated and the years left at the current write rate.

#### Scenario: Known rating
Given an SSD rated 1200 TBW has written 300 TB and writes 50 TB per year
When the projection is computed
Then it shows 25 % of rated TBW used and 18 years left

#### Scenario: Rating unknown
Given an SSD whose model has no TBW rating in drive-specs.json
When the projection is computed
Then the data written is shown and the projection is stated as unavailable because the rating is unknown
