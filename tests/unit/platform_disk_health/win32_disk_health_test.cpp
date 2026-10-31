#include "platform/disk_health/win32_disk_health.hpp"

#include <iostream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/health/analysis.hpp"
#include "core/health/reading.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {
namespace {

using health::Bus;
using health::DriveProbe;
using health::HealthReading;
using health::Source;

// Smoke test against the real machine, elevated or not: every disk is listed, every NVMe and
// SATA disk is readable (no elevation needed), and a disk without health data says why.
TEST(Win32DiskHealthSource, EveryDiskHasAHealthReadingOrAReason) {
  const Win32DiskHealthSource source;
  const Result<std::vector<DriveProbe>> probes = source.probe();
  ASSERT_TRUE(probes.has_value()) << probes.error().message;
  ASSERT_FALSE(probes->empty());

  for (const DriveProbe& probe : *probes) {
    const HealthReading reading = health::interpret(probe);
    const health::Assessment assessment = health::assess(reading, nullptr);
    std::cout << "disk " << probe.disk_number << " " << reading.model << " ["
              << health::to_string(reading.bus) << "] source " << health::to_string(reading.source)
              << ", status " << health::to_string(assessment.status)
              << (reading.problem.empty() ? "" : ", problem: " + reading.problem) << "\n";

    EXPECT_FALSE(reading.model.empty());
    if (!probe.serial.empty()) {
      EXPECT_EQ(reading.drive.find(probe.serial), std::string::npos)
          << "the serial number must not appear in the identity";
    }
    if (probe.bus == Bus::nvme) {
      EXPECT_EQ(reading.source, Source::nvme_health_log) << reading.problem;
      EXPECT_TRUE(reading.temperature_c.has_value());
      EXPECT_TRUE(reading.percentage_used.has_value());
    }
    if (probe.bus == Bus::ata) {
      EXPECT_EQ(reading.source, Source::ata_smart) << reading.problem;
      EXPECT_TRUE(reading.power_on_hours.has_value());
      EXPECT_TRUE(reading.predicts_failure.has_value());
    }
    if (reading.source == Source::none || reading.source == Source::reliability_counters) {
      EXPECT_FALSE(reading.problem.empty());
    }
    if (probe.bus == Bus::usb && reading.source != Source::nvme_health_log &&
        reading.source != Source::ata_smart && reading.source != Source::sat_pass_through) {
      EXPECT_TRUE(reading.problem.starts_with("not readable over USB")) << reading.problem;
    }
  }
}

TEST(ReliabilityCounters, ReadsOrSaysThatAdministratorRightsAreNeeded) {
  const Result<health::ReliabilityCounters> counters = reliability_counters(0);
  if (counters) {
    std::cout << "reliability counters of disk 0 readable (elevated run)\n";
    return;
  }
  std::cout << counters.error().message << "\n";
  EXPECT_TRUE(counters.error().code == Errc::access_denied ||
              counters.error().code == Errc::not_found)
      << counters.error().message;
}

}  // namespace
}  // namespace myclean::platform
