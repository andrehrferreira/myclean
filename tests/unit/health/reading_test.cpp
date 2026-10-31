#include "core/health/reading.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "health_test_support.hpp"

namespace myclean::health {
namespace {

using testing::fixture;

DriveProbe nvme_probe(const std::string& file, std::string model) {
  return DriveProbe{.disk_number = 3,
                    .model = std::move(model),
                    .serial = "S6B0NL0T000000X",
                    .firmware = "3B2QGXA7",
                    .bus = Bus::nvme,
                    .nvme_health_log = fixture(file)};
}

TEST(Reading, Samsung980ProHealthLogDecodesEveryField) {
  const HealthReading reading =
      interpret(nvme_probe("nvme_samsung_980_pro_2tb.bin", "Samsung SSD 980 PRO 2TB"));

  EXPECT_EQ(reading.source, Source::nvme_health_log);
  EXPECT_TRUE(reading.problem.empty());
  EXPECT_EQ(reading.critical_warning, 0);
  EXPECT_EQ(reading.temperature_c, 42);
  EXPECT_EQ(reading.available_spare, 95);
  EXPECT_EQ(reading.spare_threshold, 10);
  EXPECT_EQ(reading.percentage_used, 5);
  EXPECT_EQ(reading.media_errors, 498);
  EXPECT_EQ(reading.unsafe_shutdowns, 276);
  EXPECT_EQ(reading.power_on_hours, 3656);
  EXPECT_EQ(reading.bytes_written, std::int64_t{193'091'535} * 512'000);
  EXPECT_FALSE(reading.reallocated_sectors.has_value());
}

struct ExpectedNvme {
  const char* file;
  std::int64_t temperature;
  std::int64_t spare;
  std::int64_t threshold;
  std::int64_t used;
  std::int64_t units_written;
  std::int64_t hours;
  std::int64_t unsafe;
};

void expect_nvme(const ExpectedNvme& drive) {
  const HealthReading reading = interpret(nvme_probe(drive.file, "model"));
  EXPECT_EQ(reading.temperature_c, drive.temperature) << drive.file;
  EXPECT_EQ(reading.available_spare, drive.spare) << drive.file;
  EXPECT_EQ(reading.spare_threshold, drive.threshold) << drive.file;
  EXPECT_EQ(reading.percentage_used, drive.used) << drive.file;
  EXPECT_EQ(reading.bytes_written, drive.units_written * 512'000) << drive.file;
  EXPECT_EQ(reading.power_on_hours, drive.hours) << drive.file;
  EXPECT_EQ(reading.unsafe_shutdowns, drive.unsafe) << drive.file;
  EXPECT_EQ(reading.media_errors, 0) << drive.file;
  EXPECT_EQ(reading.critical_warning, 0) << drive.file;
}

TEST(Reading, EveryRecordedNvmeDriveDecodes) {
  expect_nvme({.file = "nvme_samsung_970_pro_1tb.bin",
               .temperature = 56,
               .spare = 100,
               .threshold = 10,
               .used = 10,
               .units_written = 455'575'656,
               .hours = 54'684,
               .unsafe = 512});
  expect_nvme({.file = "nvme_wd_black_sn7100_4tb.bin",
               .temperature = 41,
               .spare = 100,
               .threshold = 10,
               .used = 0,
               .units_written = 97'692'523,
               .hours = 1'197,
               .unsafe = 4});
  expect_nvme({.file = "nvme_crucial_p3_4tb.bin",
               .temperature = 45,
               .spare = 100,
               .threshold = 5,
               .used = 1,
               .units_written = 19'054'560,
               .hours = 17'132,
               .unsafe = 80});
  expect_nvme({.file = "nvme_sabrent_rocket_4_2tb.bin",
               .temperature = 42,
               .spare = 100,
               .threshold = 5,
               .used = 6,
               .units_written = 263'064'579,
               .hours = 50'561,
               .unsafe = 396});
}

TEST(Reading, NvmeCounterAbove64BitsSaturates) {
  std::vector<std::uint8_t> log = fixture("nvme_samsung_970_pro_1tb.bin");
  log.at(160 + 8) = 1;  // high half of media errors
  const HealthReading reading =
      interpret(DriveProbe{.model = "m", .bus = Bus::nvme, .nvme_health_log = std::move(log)});
  EXPECT_EQ(reading.media_errors, std::numeric_limits<std::int64_t>::max());
}

TEST(Reading, WdRedPlusSmartDataDecodesTheSataAttributes) {
  const std::vector<std::uint8_t> data = fixture("smart_wd_red_plus_10tb.bin");
  const Result<std::vector<SmartAttribute>> attributes = parse_smart_attributes(data);
  ASSERT_TRUE(attributes.has_value()) << attributes.error().message;
  EXPECT_EQ(attributes->size(), 17U);

  const HealthReading reading = interpret(DriveProbe{.disk_number = 0,
                                                     .model = "WDC WD101EFBX-68B0AN0",
                                                     .serial = "VCJ00000",
                                                     .bus = Bus::ata,
                                                     .smart_data = data,
                                                     .predicts_failure = false,
                                                     .temperature_c = 39});
  EXPECT_EQ(reading.source, Source::ata_smart);
  EXPECT_EQ(reading.reallocated_sectors, 0);
  EXPECT_EQ(reading.pending_sectors, 0);
  EXPECT_EQ(reading.uncorrectable_sectors, 0);
  EXPECT_EQ(reading.crc_errors, 0);
  EXPECT_EQ(reading.power_on_hours, 26'868);
  EXPECT_EQ(reading.temperature_c, 40);  // attribute 194, not the storage property
  EXPECT_EQ(reading.predicts_failure, false);
  EXPECT_FALSE(reading.media_errors.has_value());
}

TEST(Reading, SmartDataWithABadChecksumOrNoAttributesIsRejected) {
  std::vector<std::uint8_t> data = fixture("smart_wd_red_plus_10tb.bin");
  data.at(511) ^= 0x01U;
  EXPECT_FALSE(parse_smart_attributes(data).has_value());
  EXPECT_FALSE(parse_smart_attributes(std::vector<std::uint8_t>(512, 0)).has_value());
  EXPECT_FALSE(parse_smart_attributes(std::vector<std::uint8_t>(100, 0)).has_value());

  const HealthReading reading = interpret(DriveProbe{.model = "m",
                                                     .bus = Bus::usb,
                                                     .smart_data = data,
                                                     .smart_via_bridge = true,
                                                     .temperature_c = 30});
  EXPECT_EQ(reading.source, Source::none);
  EXPECT_EQ(reading.problem, "SMART data checksum does not match");
  EXPECT_EQ(reading.temperature_c, 30);
}

TEST(Reading, BridgeThatForwardsSatGivesTheFullSmartReadingWithoutAProblem) {
  const HealthReading reading =
      interpret(DriveProbe{.model = "ROG ESD-S1C",
                           .bus = Bus::usb,
                           .smart_data = fixture("smart_wd_red_plus_10tb.bin"),
                           .smart_via_bridge = true});
  EXPECT_EQ(reading.source, Source::sat_pass_through);
  EXPECT_TRUE(reading.problem.empty());
  EXPECT_EQ(reading.reallocated_sectors, 0);
}

TEST(Reading, BlockedBridgeFallsBackToReliabilityCounters) {
  const HealthReading reading = interpret(DriveProbe{
      .disk_number = 7,
      .model = "ROG ESD-S1C",
      .bus = Bus::usb,
      .reliability =
          ReliabilityCounters{.temperature_c = 35, .wear_percent = 2, .power_on_hours = 900},
      .problem = "not readable over USB: SAT pass-through needs administrator rights"});
  EXPECT_EQ(reading.source, Source::reliability_counters);
  EXPECT_TRUE(reading.problem.starts_with("not readable over USB"));
  EXPECT_EQ(reading.temperature_c, 35);
  EXPECT_EQ(reading.percentage_used, 2);
  EXPECT_EQ(reading.power_on_hours, 900);
}

TEST(Reading, NothingReadableHasSourceNone) {
  const HealthReading reading = interpret(DriveProbe{
      .model = "ROG ESD-S1C", .bus = Bus::usb, .problem = "not readable over USB: blocked"});
  EXPECT_EQ(reading.source, Source::none);
  EXPECT_EQ(reading.problem, "not readable over USB: blocked");
}

TEST(Reading, IdentityIsModelAndSerialHashNotTheDiskNumber) {
  const DriveProbe first{.disk_number = 0, .model = "WDC WD101EFBX-68B0AN0", .serial = "AAAA"};
  DriveProbe moved = first;
  moved.disk_number = 4;
  const DriveProbe twin{.disk_number = 1, .model = "WDC WD101EFBX-68B0AN0", .serial = "BBBB"};

  EXPECT_EQ(interpret(first).drive, interpret(moved).drive);
  EXPECT_NE(interpret(first).drive, interpret(twin).drive);
  EXPECT_EQ(interpret(first).drive, "WDC WD101EFBX-68B0AN0#" + serial_hash("AAAA"));
  EXPECT_EQ(interpret(first).drive.find("AAAA"), std::string::npos);
}

TEST(Reading, SerialHashIsFixedFnv1aOfTheTrimmedSerial) {
  EXPECT_EQ(serial_hash(""), "cbf29ce484222325");  // the FNV-1a offset basis
  EXPECT_EQ(serial_hash("a"), "af63dc4c8601ec8c");
  EXPECT_EQ(serial_hash("  a  "), serial_hash("a"));
}

TEST(Reading, EnumTextFormsRoundTrip) {
  for (const Source source : {Source::nvme_health_log, Source::ata_smart, Source::sat_pass_through,
                              Source::reliability_counters, Source::none}) {
    EXPECT_EQ(parse_source(to_string(source)), source);
  }
  for (const Bus bus : {Bus::nvme, Bus::ata, Bus::usb, Bus::other}) {
    EXPECT_EQ(parse_bus(to_string(bus)), bus);
  }
  EXPECT_FALSE(parse_source("smart").has_value());
}

}  // namespace
}  // namespace myclean::health
