#include "core/drives/analysis.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/health/analysis.hpp"
#include "core/health/collector.hpp"
#include "core/health/reading.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/volume_info.hpp"
#include "drives_fakes.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::drives {
namespace {

using namespace std::chrono_literals;
using namespace myclean::literals;
using testing::FakeLayout;
using testing::FakeLog;
using testing::FakeReader;
using testing::FakeSeekPenalty;
using testing::FakeTrim;
using testing::file;
using testing::hdd;
using testing::optimized;
using testing::scattered;
using testing::ssd;

const WallTime now = std::chrono::sys_days{std::chrono::October / 31 / 2026} + 12h;

health::DriveHealth drive(std::uint32_t disk, health::Bus bus, std::string model,
                          health::Status status = health::Status::good) {
  health::DriveHealth result;
  result.reading.disk_number = disk;
  result.reading.bus = bus;
  result.reading.model = std::move(model);
  result.assessment.status = status;
  if (status != health::Status::good) {
    result.assessment.reasons = {"1 new pending sector"};
  }
  return result;
}

telemetry::VolumeStatus volume(std::string root, std::vector<std::uint32_t> disks) {
  return {.id = R"(\\?\Volume{1}\)",
          .mount_points = {std::move(root)},
          .size = 1000_GiB,
          .free = 500_GiB,
          .disks = std::move(disks)};
}

bool mentions(const VolumeAnalysis& analysis, const std::string& text) {
  return std::ranges::any_of(analysis.reasons, [&](const std::string& reason) {
    return reason.find(text) != std::string::npos;
  });
}

TEST(DriveStates, JoinVolumesWithMediumTrimAndHealth) {
  const std::vector<telemetry::VolumeStatus> volumes = {volume("D:\\", {4}), volume("K:\\", {0}),
                                                        volume("E:\\", {7}), volume("V:\\", {9}),
                                                        volume(R"(\\?\Volume{2}\)", {4})};
  health::HealthView view;
  view.drives = {drive(0, health::Bus::ata, "WDC WD101EFBX-68B0AN0", health::Status::warning),
                 drive(4, health::Bus::nvme, "WD_BLACK SN7100 4TB"),
                 drive(7, health::Bus::usb, "ROG ESD-S1C"),
                 drive(9, health::Bus::other, "Virtual Disk")};
  FakeSeekPenalty seek;
  seek.penalty = {{0, true}, {4, false}, {7, false}};
  FakeTrim trim;
  trim.supported = {{0, false}, {4, true}, {7, true}};

  const std::vector<DriveState> states = drive_states(volumes, &view, seek, trim);
  ASSERT_EQ(states.size(), 4U);  // the volume without a drive letter is left out
  EXPECT_EQ(states[0].media, Media::ssd);
  EXPECT_EQ(states[0].drive_class, placement::DriveClass::nvme);
  EXPECT_EQ(states[0].trim_supported, true);
  EXPECT_TRUE(states[0].healthy);
  EXPECT_EQ(states[0].ntfs_trim_enabled, true);
  EXPECT_EQ(states[1].media, Media::hdd);
  EXPECT_EQ(states[1].trim_supported, false);
  EXPECT_FALSE(states[1].healthy);
  EXPECT_NE(states[1].health_note.find("pending sector"), std::string::npos);
  EXPECT_EQ(states[2].drive_class, placement::DriveClass::usb);
  EXPECT_EQ(states[2].media, Media::ssd);  // a USB SSD is still retrimmed
  EXPECT_EQ(states[3].media, Media::unknown);
  EXPECT_FALSE(states[3].trim_supported.has_value());
}

TEST(Recommend, RetrimsAnSsdWhoseLastRetrimIsOlderThan30Days) {
  VolumeAnalysis analysis{.drive = ssd("D:\\", 4), .last = {.retrim = now - 40 * 24h}};
  recommend(analysis, now, Thresholds{});
  EXPECT_EQ(analysis.recommended, Maintenance::retrim);
  EXPECT_TRUE(mentions(analysis, "40 days ago"));

  analysis.last.retrim = now - 2 * 24h;
  recommend(analysis, now, Thresholds{});
  EXPECT_EQ(analysis.recommended, Maintenance::none);
  EXPECT_TRUE(mentions(analysis, "nothing to do"));
}

TEST(Recommend, RetrimsAnSsdWithoutRetrimInTheLog) {
  VolumeAnalysis analysis{.drive = ssd("W:\\", 5), .last = {.log_since = now - 24 * 24h}};
  recommend(analysis, now, Thresholds{});
  EXPECT_EQ(analysis.recommended, Maintenance::retrim);
  EXPECT_TRUE(mentions(analysis, "reaches back to 2026-10-07"));

  analysis.history_note = "access denied";
  recommend(analysis, now, Thresholds{});
  EXPECT_EQ(analysis.recommended, Maintenance::retrim);
  EXPECT_TRUE(mentions(analysis, "could not be read"));
}

TEST(Recommend, AnSsdIsNeverDefragmentedAndOneRefusingTrimGetsNothing) {
  VolumeAnalysis analysis{.drive = ssd("W:\\", 5), .last = {.retrim = now - 1 * 24h}};
  analysis.fragmentation = FragmentationSample{
      .files = 100, .bytes = 100_GiB, .fragmented_files = 90, .fragmented_bytes = 90_GiB};
  recommend(analysis, now, Thresholds{});
  EXPECT_EQ(analysis.needed, Maintenance::none);

  analysis.drive.trim_supported = false;
  analysis.last.retrim.reset();
  recommend(analysis, now, Thresholds{});
  EXPECT_EQ(analysis.needed, Maintenance::none);
  EXPECT_TRUE(mentions(analysis, "does not accept TRIM"));
}

TEST(Recommend, DefragmentsAnHddAboveTheThresholdOnly) {
  VolumeAnalysis analysis{.drive = hdd("K:\\", 0)};
  analysis.fragmentation = FragmentationSample{
      .files = 50, .bytes = 100_GiB, .fragmented_files = 10, .fragmented_bytes = 25_GiB};
  recommend(analysis, now, Thresholds{});
  EXPECT_EQ(analysis.recommended, Maintenance::defrag);
  EXPECT_TRUE(mentions(analysis, "25.0 % of the sampled bytes"));

  analysis.fragmentation->fragmented_bytes = 5_GiB;
  recommend(analysis, now, Thresholds{});
  EXPECT_EQ(analysis.recommended, Maintenance::none);

  analysis.fragmentation = FragmentationSample{
      .files = 3, .bytes = 3_GiB, .fragmented_files = 3, .fragmented_bytes = 3_GiB};
  recommend(analysis, now, Thresholds{});
  EXPECT_EQ(analysis.recommended, Maintenance::none);
  EXPECT_TRUE(mentions(analysis, "too few to judge"));
}

TEST(Recommend, AnUnhealthyDriveIsNotMaintainedButTheNeedIsShown) {
  VolumeAnalysis analysis{.drive = hdd("K:\\", 0)};
  analysis.drive.healthy = false;
  analysis.drive.health_note =
      "disk 0 (WDC WD101EFBX-68B0AN0) health is warning; 1 new pending sector";
  analysis.fragmentation = FragmentationSample{
      .files = 50, .bytes = 100_GiB, .fragmented_files = 10, .fragmented_bytes = 25_GiB};
  recommend(analysis, now, Thresholds{});
  EXPECT_EQ(analysis.needed, Maintenance::defrag);
  EXPECT_EQ(analysis.recommended, Maintenance::none);
  EXPECT_TRUE(mentions(analysis, "pending sector"));
}

TEST(Recommend, AnUnknownMediumGetsNothing) {
  VolumeAnalysis analysis{.drive = ssd("V:\\", 9)};
  analysis.drive.media = Media::unknown;
  recommend(analysis, now, Thresholds{});
  EXPECT_EQ(analysis.needed, Maintenance::none);
  EXPECT_TRUE(mentions(analysis, "medium is unknown"));
}

TEST(Analyze, SamplesHddsOnlyAndReadsTheLogOnce) {
  FakeReader reader;
  reader.listings[L"K:\\"] = {file("a.bin", (1_GiB).count())};
  FakeLayout layout;
  layout.runs[L"K:\\a.bin"] = scattered(200);
  FakeLog log;
  log.logged = std::vector<OptimizerEvent>{
      optimized(now - 3 * 24h, "SSD AR (D:)", OptimizationKind::retrim),
      optimized(now - 2 * 24h, "Backup 1 (K:)", OptimizationKind::defrag)};
  const fakes::FakeClock clock{now};
  const AnalysisTools tools{.reader = &reader, .layout = &layout, .log = &log, .clock = &clock};
  const std::vector<DriveState> drives = {ssd("D:\\", 4), hdd("K:\\", 0)};

  const std::vector<VolumeAnalysis> analyses = analyze(drives, tools, AnalysisOptions{});
  ASSERT_EQ(analyses.size(), 2U);
  EXPECT_EQ(analyses[0].last.retrim, now - 3 * 24h);
  EXPECT_FALSE(analyses[0].fragmentation.has_value());
  EXPECT_EQ(analyses[0].recommended, Maintenance::none);
  ASSERT_TRUE(analyses[1].fragmentation.has_value());
  EXPECT_EQ(analyses[1].fragmentation.value_or(FragmentationSample{}).files, 1U);
  EXPECT_EQ(analyses[1].last.defrag, now - 2 * 24h);
  EXPECT_FALSE(analyses[1].free_space.has_value());
  EXPECT_NE(analyses[1].free_space_note.find("Win32 error 5"), std::string::npos);
  EXPECT_EQ(layout.bitmap_reads, std::vector<std::filesystem::path>{L"K:\\"});
}

TEST(Analyze, AnUnreadableLogIsNotedOnEveryVolume) {
  FakeReader reader;
  FakeLayout layout;
  FakeLog log;
  log.logged = fail(Errc::io_error, "the event log service is stopped");
  const fakes::FakeClock clock{now};
  const AnalysisTools tools{.reader = &reader, .layout = &layout, .log = &log, .clock = &clock};
  const std::vector<DriveState> drives = {ssd("D:\\", 4)};

  const std::vector<VolumeAnalysis> analyses = analyze(drives, tools, AnalysisOptions{});
  ASSERT_EQ(analyses.size(), 1U);
  EXPECT_EQ(analyses[0].history_note, "the event log service is stopped");
  EXPECT_EQ(analyses[0].recommended, Maintenance::retrim);
}

}  // namespace
}  // namespace myclean::drives
