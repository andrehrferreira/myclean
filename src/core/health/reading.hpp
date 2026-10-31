#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"

namespace myclean::health {

// How the disk is attached, as Windows reports it.
enum class Bus { nvme, ata, usb, other };

// Where a health reading came from. Stored with every snapshot.
enum class Source {
  nvme_health_log,       // NVMe SMART / Health Information log page 02h
  ata_smart,             // ATA SMART READ DATA on a SATA disk
  sat_pass_through,      // ATA SMART READ DATA through a USB bridge (SCSI/ATA Translation)
  reliability_counters,  // Windows storage reliability counters only
  none,                  // nothing could be read
};

[[nodiscard]] std::string_view to_string(Bus bus);
[[nodiscard]] std::optional<Bus> parse_bus(std::string_view text);
[[nodiscard]] std::string_view to_string(Source source);
[[nodiscard]] std::optional<Source> parse_source(std::string_view text);

// Windows' storage reliability counters (MSFT_StorageReliabilityCounter); each may be absent.
struct ReliabilityCounters {
  std::optional<std::int64_t> temperature_c;
  std::optional<std::int64_t> wear_percent;
  std::optional<std::int64_t> power_on_hours;
  std::optional<std::int64_t> read_errors_uncorrected;
  std::optional<std::int64_t> write_errors_uncorrected;
};

// What the platform read from one physical disk, before interpretation. Byte blocks are kept raw
// so that their layout is decoded (and tested against recorded fixtures) in core.
struct DriveProbe {
  std::uint32_t disk_number = 0;  // changes when disks are added or removed; not an identity
  std::string model;              // vendor and product as Windows reports them
  std::string serial;             // only its hash leaves this struct
  std::string firmware;
  Bus bus = Bus::other;
  std::vector<std::uint8_t> nvme_health_log;  // the 512-byte log page 02h, or empty
  std::vector<std::uint8_t> smart_data;       // the 512-byte SMART READ DATA sector, or empty
  bool smart_via_bridge = false;              // smart_data came through a USB bridge
  std::optional<bool> predicts_failure;       // the drive's own SMART failure prediction
  std::optional<std::int64_t> temperature_c;  // StorageDeviceTemperatureProperty
  std::optional<ReliabilityCounters> reliability;
  // Why a health source could not be read, e.g. "not readable over USB: ...". Empty when every
  // source this bus offers was read.
  std::string problem;
};

// One interpreted health reading of one physical disk. Counters the source does not provide
// are absent, never zero.
struct HealthReading {
  std::string drive;  // stable identity: "<model>#<serial hash>"
  std::string model;
  std::string firmware;
  std::uint32_t disk_number = 0;
  Bus bus = Bus::other;
  Source source = Source::none;
  std::string problem;  // see DriveProbe::problem

  std::optional<std::int64_t> temperature_c;
  // NVMe
  std::optional<std::int64_t> critical_warning;  // bit field of the health log, 0 = none
  std::optional<std::int64_t> percentage_used;   // wear, may exceed 100; also reliability "wear"
  std::optional<std::int64_t> available_spare;   // %
  std::optional<std::int64_t> spare_threshold;   // %
  std::optional<std::int64_t> media_errors;      // media and data integrity errors
  std::optional<std::int64_t> unsafe_shutdowns;
  std::optional<std::int64_t> bytes_written;  // host writes
  std::optional<std::int64_t> power_on_hours;
  // ATA
  std::optional<std::int64_t> reallocated_sectors;    // attribute 5
  std::optional<std::int64_t> pending_sectors;        // attribute 197
  std::optional<std::int64_t> uncorrectable_sectors;  // attribute 198
  std::optional<std::int64_t> crc_errors;             // attribute 199
  std::optional<bool> predicts_failure;
  // Reliability counters
  std::optional<std::int64_t> read_errors_uncorrected;
  std::optional<std::int64_t> write_errors_uncorrected;
};

// The first 16 hex digits of the FNV-1a 64-bit hash of the trimmed serial number: stable across
// builds and runs, and the serial itself is never stored.
[[nodiscard]] std::string serial_hash(std::string_view serial);

// One entry of the ATA SMART attribute table.
struct SmartAttribute {
  std::uint8_t id = 0;
  std::uint8_t current = 0;  // normalized value
  std::uint8_t worst = 0;
  std::uint64_t raw = 0;  // the 48-bit raw value
};

// The attribute table of a 512-byte SMART READ DATA sector (30 entries of 12 bytes from offset
// 2; id 0 marks an unused entry). Fails with invalid_data on a block of another size, a wrong
// checksum (byte 511 makes the sum of all bytes 0 mod 256) or a table without attributes.
[[nodiscard]] Result<std::vector<SmartAttribute>> parse_smart_attributes(
    std::span<const std::uint8_t> data);

// Interprets `probe`: decodes the NVMe health log or the SMART attributes, falls back to the
// reliability counters, and names the source. A block of the wrong size is reported in
// `problem` and ignored.
[[nodiscard]] HealthReading interpret(const DriveProbe& probe);

}  // namespace myclean::health
