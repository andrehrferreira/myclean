#include "core/health/reading.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/enum_names.hpp"
#include "core/model/result.hpp"

namespace myclean::health {

namespace {

constexpr std::array<std::string_view, 4> bus_names{"nvme", "ata", "usb", "other"};
constexpr std::array<std::string_view, 5> source_names{
    "nvme_health_log", "ata_smart", "sat_pass_through", "reliability_counters", "none"};

constexpr std::size_t block_size = 512;
constexpr std::int64_t int64_max = std::numeric_limits<std::int64_t>::max();

// NVMe SMART / Health Information log (NVMe Base Specification, log page 02h), little-endian.
constexpr std::size_t nvme_critical_warning = 0;
constexpr std::size_t nvme_temperature = 1;  // 2 bytes, kelvin
constexpr std::size_t nvme_available_spare = 3;
constexpr std::size_t nvme_spare_threshold = 4;
constexpr std::size_t nvme_percentage_used = 5;
constexpr std::size_t nvme_data_units_written = 48;  // 16 bytes each from here on
constexpr std::size_t nvme_power_on_hours = 128;
constexpr std::size_t nvme_unsafe_shutdowns = 144;
constexpr std::size_t nvme_media_errors = 160;
// A data unit is 1000 sectors of 512 bytes.
constexpr std::int64_t nvme_data_unit = 512'000;
constexpr std::int64_t kelvin_offset = 273;

// ATA SMART READ DATA: revision (2 bytes), then 30 attributes of 12 bytes.
constexpr std::size_t smart_first_attribute = 2;
constexpr std::size_t smart_attribute_size = 12;
constexpr std::size_t smart_attribute_count = 30;

constexpr std::uint8_t attribute_reallocated = 5;
constexpr std::uint8_t attribute_power_on_hours = 9;
constexpr std::uint8_t attribute_airflow_temperature = 190;
constexpr std::uint8_t attribute_temperature = 194;
constexpr std::uint8_t attribute_pending = 197;
constexpr std::uint8_t attribute_uncorrectable = 198;
constexpr std::uint8_t attribute_crc_errors = 199;

std::uint64_t little_endian(std::span<const std::uint8_t> bytes) {
  std::uint64_t value = 0;
  for (std::size_t index = bytes.size(); index > 0; --index) {
    value = (value << 8U) | bytes[index - 1];
  }
  return value;
}

// A 128-bit NVMe counter, saturated to the int64 range.
std::int64_t counter128(std::span<const std::uint8_t> log, std::size_t offset) {
  const std::uint64_t low = little_endian(log.subspan(offset, 8));
  const std::uint64_t high = little_endian(log.subspan(offset + 8, 8));
  if (high != 0 || low > static_cast<std::uint64_t>(int64_max)) {
    return int64_max;
  }
  return static_cast<std::int64_t>(low);
}

void append_problem(std::string& problem, std::string_view text) {
  if (!problem.empty()) {
    problem += "; ";
  }
  problem += text;
}

void read_nvme(std::span<const std::uint8_t> log, HealthReading& reading) {
  reading.critical_warning = log[nvme_critical_warning];
  if (const std::uint64_t kelvin = little_endian(log.subspan(nvme_temperature, 2)); kelvin != 0) {
    reading.temperature_c = static_cast<std::int64_t>(kelvin) - kelvin_offset;
  }
  reading.available_spare = log[nvme_available_spare];
  reading.spare_threshold = log[nvme_spare_threshold];
  reading.percentage_used = log[nvme_percentage_used];
  const std::int64_t units = counter128(log, nvme_data_units_written);
  reading.bytes_written = units > int64_max / nvme_data_unit ? int64_max : units * nvme_data_unit;
  reading.power_on_hours = counter128(log, nvme_power_on_hours);
  reading.unsafe_shutdowns = counter128(log, nvme_unsafe_shutdowns);
  reading.media_errors = counter128(log, nvme_media_errors);
}

void read_smart(const std::vector<SmartAttribute>& attributes, HealthReading& reading) {
  std::optional<std::int64_t> airflow;
  for (const SmartAttribute& attribute : attributes) {
    const auto raw = static_cast<std::int64_t>(attribute.raw);
    const std::int64_t low16 = raw & 0xFFFF;
    const std::int64_t low32 = raw & 0xFFFF'FFFF;
    switch (attribute.id) {
      case attribute_reallocated:
        reading.reallocated_sectors = raw;
        break;
      case attribute_power_on_hours:
        reading.power_on_hours = low32;  // some vendors keep minutes in the upper bytes
        break;
      case attribute_temperature:
        reading.temperature_c = low16 & 0xFF;  // upper bytes: lowest and highest seen
        break;
      case attribute_airflow_temperature:
        airflow = low16 & 0xFF;
        break;
      case attribute_pending:
        reading.pending_sectors = raw;
        break;
      case attribute_uncorrectable:
        reading.uncorrectable_sectors = raw;
        break;
      case attribute_crc_errors:
        reading.crc_errors = raw;
        break;
      default:
        break;
    }
  }
  if (!reading.temperature_c) {
    reading.temperature_c = airflow;
  }
}

void read_reliability(const ReliabilityCounters& counters, HealthReading& reading) {
  reading.temperature_c = counters.temperature_c;
  reading.percentage_used = counters.wear_percent;
  reading.power_on_hours = counters.power_on_hours;
  reading.read_errors_uncorrected = counters.read_errors_uncorrected;
  reading.write_errors_uncorrected = counters.write_errors_uncorrected;
}

}  // namespace

std::string_view to_string(Bus bus) {
  return detail::enum_name(bus_names, bus);
}

std::optional<Bus> parse_bus(std::string_view text) {
  return detail::enum_from_name<Bus>(bus_names, text);
}

std::string_view to_string(Source source) {
  return detail::enum_name(source_names, source);
}

std::optional<Source> parse_source(std::string_view text) {
  return detail::enum_from_name<Source>(source_names, text);
}

std::string serial_hash(std::string_view serial) {
  const std::size_t first = serial.find_first_not_of(' ');
  serial = first == std::string_view::npos ? std::string_view{} : serial.substr(first);
  serial = serial.substr(0, serial.find_last_not_of(' ') + 1);
  constexpr std::uint64_t fnv_offset = 14'695'981'039'346'656'037ULL;
  constexpr std::uint64_t fnv_prime = 1'099'511'628'211ULL;
  std::uint64_t hash = fnv_offset;
  for (const char character : serial) {
    hash = (hash ^ static_cast<std::uint8_t>(character)) * fnv_prime;
  }
  return std::format("{:016x}", hash);
}

Result<std::vector<SmartAttribute>> parse_smart_attributes(std::span<const std::uint8_t> data) {
  if (data.size() != block_size) {
    return fail(Errc::invalid_data,
                std::format("SMART data is {} bytes, expected {}", data.size(), block_size));
  }
  std::uint8_t sum = 0;
  for (const std::uint8_t byte : data) {
    sum = static_cast<std::uint8_t>(sum + byte);
  }
  if (sum != 0) {
    return fail(Errc::invalid_data, "SMART data checksum does not match");
  }
  std::vector<SmartAttribute> attributes;
  for (std::size_t index = 0; index < smart_attribute_count; ++index) {
    const std::span<const std::uint8_t> entry =
        data.subspan(smart_first_attribute + (index * smart_attribute_size), smart_attribute_size);
    if (entry[0] == 0) {
      continue;
    }
    // id, flags (2 bytes), current, worst, raw (6 bytes), reserved
    attributes.push_back(SmartAttribute{.id = entry[0],
                                        .current = entry[3],
                                        .worst = entry[4],
                                        .raw = little_endian(entry.subspan(5, 6))});
  }
  if (attributes.empty()) {
    return fail(Errc::invalid_data, "SMART data holds no attributes");
  }
  return attributes;
}

HealthReading interpret(const DriveProbe& probe) {
  HealthReading reading{.drive = probe.model + "#" + serial_hash(probe.serial),
                        .model = probe.model,
                        .firmware = probe.firmware,
                        .disk_number = probe.disk_number,
                        .bus = probe.bus,
                        .problem = probe.problem};
  if (!probe.nvme_health_log.empty()) {
    if (probe.nvme_health_log.size() == block_size) {
      read_nvme(probe.nvme_health_log, reading);
      reading.source = Source::nvme_health_log;
    } else {
      append_problem(reading.problem, std::format("NVMe health log is {} bytes, expected {}",
                                                  probe.nvme_health_log.size(), block_size));
    }
  }
  if (reading.source == Source::none && !probe.smart_data.empty()) {
    if (const Result<std::vector<SmartAttribute>> attributes =
            parse_smart_attributes(probe.smart_data)) {
      read_smart(*attributes, reading);
      reading.predicts_failure = probe.predicts_failure;
      reading.source = probe.smart_via_bridge ? Source::sat_pass_through : Source::ata_smart;
    } else {
      append_problem(reading.problem, attributes.error().message);
    }
  }
  if (reading.source == Source::none && probe.reliability) {
    read_reliability(*probe.reliability, reading);
    reading.source = Source::reliability_counters;
  }
  if (!reading.temperature_c) {
    reading.temperature_c = probe.temperature_c;
  }
  return reading;
}

}  // namespace myclean::health
