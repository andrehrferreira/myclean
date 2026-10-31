// The drive-maintenance readers against this machine, read-only: the optimizer log, TRIM
// support, the power source, cluster runs of real files and a small, bounded fragmentation
// sample of one SSD folder and one HDD. Nothing here defragments, retrims or changes a setting.
//
// The per-drive report of this machine reads every volume and takes up to a minute per HDD, so
// it runs only on request:
//   myclean_platform_defrag_tests --gtest_also_run_disabled_tests --gtest_filter=*Report*

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/drives/analysis.hpp"
#include "core/drives/history.hpp"
#include "core/drives/layout.hpp"
#include "core/health/analysis.hpp"
#include "core/health/collector.hpp"
#include "core/health/reading.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "platform/defrag/system_sources.hpp"
#include "platform/defrag/volume_layout.hpp"
#include "platform/disk/win32_volume_info.hpp"
#include "platform/disk_health/win32_disk_health.hpp"
#include "platform/scan/directory_reader.hpp"
#include "platform/settings/seek_penalty.hpp"

namespace myclean::platform {
namespace {

using namespace std::chrono_literals;

std::filesystem::path this_executable() {
  std::array<wchar_t, MAX_PATH> name{};
  const DWORD length = ::GetModuleFileNameW(nullptr, name.data(), MAX_PATH);
  return std::filesystem::path{std::wstring(name.data(), length)};
}

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

std::string gb(Bytes bytes) {
  return std::format("{:.1f} GB", static_cast<double>(bytes.count()) / (1ULL << 30U));
}

std::string date(std::optional<WallTime> time) {
  return time
             ? std::format("{:%Y-%m-%d %H:%M} UTC", std::chrono::floor<std::chrono::minutes>(*time))
             : std::string("none in the log");
}

// The first of K:, L: whose disk has a seek penalty; empty when this machine has none.
std::filesystem::path hdd_root() {
  const Win32VolumeInfo info;
  const Win32SeekPenalty seek;
  const Result<std::vector<telemetry::VolumeStatus>> volumes = info.volumes();
  if (!volumes) {
    return {};
  }
  for (const char* root : {"K:\\", "L:\\"}) {
    for (const telemetry::VolumeStatus& volume : *volumes) {
      for (const std::string& point : volume.mount_points) {
        if (point == root && !volume.disks.empty() &&
            seek.seek_penalty(volume.disks.front()) == true) {
          return std::filesystem::path{root};
        }
      }
    }
  }
  return {};
}

TEST(Win32OptimizerLog, ReadsTheDefragEventsWithoutElevation) {
  const Win32OptimizerLog log;
  const Result<std::vector<drives::OptimizerEvent>> events = log.events();
  ASSERT_TRUE(events.has_value()) << events.error().message;
  for (const drives::OptimizerEvent& event : *events) {
    EXPECT_TRUE(event.id == drives::event_optimized || event.id == drives::event_failed);
    EXPECT_GT(event.time, WallTime{});
  }
  if (!events->empty()) {
    // This Windows build logs the result and the operation code as binary data.
    EXPECT_GE(events->front().data.size(), 8U);
  }
}

TEST(Win32TrimSupport, ReadsTheNtfsSettingAndEveryDiskAnswersOrIsUnknown) {
  const Win32TrimSupport trim;
  EXPECT_TRUE(trim.ntfs_trim_enabled().has_value());
  const Win32VolumeInfo info;
  const Result<std::vector<telemetry::VolumeStatus>> volumes = info.volumes();
  ASSERT_TRUE(volumes.has_value());
  for (const telemetry::VolumeStatus& volume : *volumes) {
    for (const std::uint32_t disk : volume.disks) {
      static_cast<void>(trim.trim_supported(disk));
    }
  }
}

TEST(Win32PowerSource, KnowsWhetherTheComputerRunsOnBattery) {
  const Win32PowerSource power;
  EXPECT_TRUE(power.on_battery().has_value());
}

TEST(Win32VolumeLayout, ReadsTheClusterRunsOfAFileWithoutElevation) {
  const Win32VolumeLayout layout;
  const Result<std::vector<drives::Extent>> extents = layout.extents(this_executable());
  ASSERT_TRUE(extents.has_value()) << extents.error().message;
  EXPECT_GE(drives::count_fragments(*extents), 1U);
  const Result<std::vector<drives::Extent>> missing =
      layout.extents(this_executable().parent_path() / "no-such-file.bin");
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, Errc::not_found);
}

TEST(Win32VolumeLayout, TheBitmapIsReadOrRefusedWithoutAdministratorRights) {
  const Win32VolumeLayout layout;
  const std::filesystem::path root = this_executable().root_path();
  const Result<drives::FreeSpaceLayout> free = layout.free_space(root, 2s);
  if (free) {
    EXPECT_GT(free->free.count(), 0U);
    std::cout << utf8(root) << " bitmap: " << gb(free->free) << " free in " << free->free_runs
              << " runs\n";
  } else {
    EXPECT_EQ(free.error().code, Errc::access_denied) << free.error().message;
  }
}

TEST(SampleFragmentation, ABoundedSampleOfAnSsdFolderAndAnHdd) {
  const BackgroundPriority background;
  EXPECT_TRUE(background.active());
  const Win32DirectoryReader reader;
  const Win32VolumeLayout layout;
  const os::SystemClock clock;

  const drives::FragmentationSample ssd = drives::sample_fragmentation(
      this_executable().parent_path(), reader, layout, clock,
      drives::SampleBudget{
          .min_file_size = Bytes{1ULL << 20U}, .max_folders = 50, .max_files = 20, .max_time = 5s});
  EXPECT_GE(ssd.files, 1U);  // at least this test executable
  EXPECT_EQ(ssd.errors, 0U);

  const std::filesystem::path hdd = hdd_root();
  if (hdd.empty()) {
    GTEST_SKIP() << "no HDD at K: or L:";
  }
  const auto started = std::chrono::steady_clock::now();
  const drives::FragmentationSample sample = drives::sample_fragmentation(
      hdd, reader, layout, clock,
      drives::SampleBudget{.max_folders = 200, .max_files = 20, .max_time = 10s});
  EXPECT_LT(std::chrono::steady_clock::now() - started, 30s);
  std::cout << utf8(hdd) << " sample: " << sample.files << " files, " << gb(sample.bytes) << ", "
            << sample.fragments_per_file() << " fragments per file, " << sample.fragmented_percent()
            << " % fragmented, " << sample.errors << " errors\n";
}

std::shared_ptr<const health::HealthView> read_health() {
  const Win32DiskHealthSource source;
  auto view = std::make_shared<health::HealthView>();
  if (Result<std::vector<health::DriveProbe>> probes = source.probe(); probes) {
    for (const health::DriveProbe& probe : *probes) {
      health::DriveHealth drive;
      drive.reading = health::interpret(probe);
      drive.assessment = health::assess(drive.reading, nullptr);
      view->drives.push_back(std::move(drive));
    }
  }
  return view;
}

std::string tri_state(std::optional<bool> value, const char* yes, const char* no) {
  if (!value) {
    return "unknown";
  }
  return *value ? yes : no;
}

void print(const drives::VolumeAnalysis& volume) {
  const drives::DriveState& drive = volume.drive;
  std::cout << utf8(drive.root) << " " << to_string(drive.drive_class) << " "
            << to_string(drive.media) << ", " << gb(drive.free) << " free of " << gb(drive.size)
            << ", TRIM " << tri_state(drive.trim_supported, "supported", "not supported")
            << ", NTFS TRIM " << tri_state(drive.ntfs_trim_enabled, "on", "off")
            << (drive.healthy ? ", healthy" : ", not healthy: " + drive.health_note) << "\n";
  std::cout << "  last retrim " << date(volume.last.retrim) << ", last defrag "
            << date(volume.last.defrag) << ", log since " << date(volume.last.log_since)
            << volume.history_note << "\n";
  if (const std::optional<drives::FragmentationSample>& sample = volume.fragmentation) {
    std::cout << std::format(
        "  sample: {} folders, {} files of 64 MiB+ ({}), {:.1f} fragments per file, {} "
        "fragmented ({:.1f} % of bytes), {} errors{}\n",
        sample->folders, sample->files, gb(sample->bytes), sample->fragments_per_file(),
        sample->fragmented_files, sample->fragmented_percent(), sample->errors,
        sample->stopped ? ", budget reached" : "");
    if (const std::optional<drives::SampledFile>& worst = sample->worst) {
      std::cout << "  most fragmented: " << utf8(worst->path) << " (" << worst->fragments
                << " fragments, " << gb(worst->size) << ")\n";
    }
  }
  if (const std::optional<drives::FreeSpaceLayout>& free = volume.free_space) {
    std::cout << std::format("  free space: {} runs, largest {}, {} in runs under 64 MiB\n",
                             free->free_runs, gb(free->largest_free_run),
                             gb(free->free_in_small_runs));
  } else if (!volume.free_space_note.empty()) {
    std::cout << "  free space: " << volume.free_space_note << "\n";
  }
  std::cout << "  needed " << to_string(volume.needed) << ", recommended "
            << to_string(volume.recommended) << "\n";
  for (const std::string& reason : volume.reasons) {
    std::cout << "    - " << reason << "\n";
  }
}

TEST(MachineReport, DISABLED_ReportsTheMaintenanceStateOfEveryDrive) {
  const BackgroundPriority background;
  const Win32VolumeInfo info;
  const Win32SeekPenalty seek;
  const Win32TrimSupport trim;
  const drives::LiveDriveStates states{info, seek, trim, read_health};
  const Result<std::vector<drives::DriveState>> drives = states.drives();
  ASSERT_TRUE(drives.has_value()) << drives.error().message;

  const Win32DirectoryReader reader;
  const Win32VolumeLayout layout;
  const Win32OptimizerLog log;
  const os::SystemClock clock;
  const drives::AnalysisTools tools{
      .reader = &reader, .layout = &layout, .log = &log, .clock = &clock};
  const drives::AnalysisOptions options{
      .sample = {.max_folders = 5'000, .max_files = 500, .max_time = 60s}, .bitmap_budget = 20s};
  for (const drives::VolumeAnalysis& volume : drives::analyze(*drives, tools, options)) {
    print(volume);
  }
}
}  // namespace
}  // namespace myclean::platform
