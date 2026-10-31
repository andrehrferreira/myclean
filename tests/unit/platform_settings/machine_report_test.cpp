// The read-only report of this machine's placement state: every cache with its location and
// size and the proposed target, and the system space settings with their recommendations.
// Measuring TEMP and the caches takes minutes, so it runs only on request:
//   myclean_platform_settings_tests --gtest_also_run_disabled_tests --gtest_filter=*Report*

#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/health/analysis.hpp"
#include "core/health/collector.hpp"
#include "core/health/reading.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/relocation.hpp"
#include "core/placement/settings.hpp"
#include "core/placement/system_space.hpp"
#include "platform/disk/win32_volume_info.hpp"
#include "platform/disk_health/win32_disk_health.hpp"
#include "platform/fs/win32_file_system.hpp"
#include "platform/settings/process_runner.hpp"
#include "platform/settings/seek_penalty.hpp"
#include "platform/settings/system_space.hpp"
#include "platform/settings/tree_copier.hpp"
#include "platform/settings/user_settings.hpp"

namespace myclean::platform {
namespace {

std::string gb(Bytes bytes) {
  return std::format("{:.2f} GB", static_cast<double>(bytes.count()) / (1ULL << 30U));
}

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
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

// `view` as the health collector's latest view.
placement::LiveDriveFacts::HealthLatest latest(
    const std::shared_ptr<const health::HealthView>& view) {
  return [view] { return view; };
}

void print(const std::string& name, const Result<placement::SettingSummary>& summary) {
  if (!summary) {
    std::cout << name << ": " << summary.error().message << "\n";
    return;
  }
  std::cout << name << ": " << summary->current << " -> " << summary->proposed << ", costs "
            << gb(summary->cost) << (summary->restart_required ? ", restart needed" : "") << "\n";
  for (const std::string& command : summary->commands) {
    std::cout << "  apply: " << command << "\n";
  }
  for (const std::string& command : summary->undo_commands) {
    std::cout << "  undo:  " << command << "\n";
  }
}

TEST(MachineReport, DISABLED_ReportsThePlacementStateOfThisMachine) {
  Win32UserEnvironment environment;
  Win32TextFiles files;
  Win32TreeCopier copier;
  Win32FileSystem file_system;
  const Win32VolumeInfo volume_info;
  const Win32SeekPenalty seek;
  const placement::LiveDriveFacts drives{volume_info, seek, latest(read_health())};
  const placement::Tools tools{.environment = &environment,
                               .files = &files,
                               .copier = &copier,
                               .file_system = &file_system,
                               .drives = &drives};

  const Result<std::vector<placement::TargetVolume>> volumes = drives.volumes();
  ASSERT_TRUE(volumes.has_value());
  for (const placement::TargetVolume& volume : *volumes) {
    std::cout << utf8(volume.root) << " " << to_string(volume.drive_class) << ", "
              << gb(volume.free) << " free of " << gb(volume.size)
              << (volume.healthy ? ", healthy" : ", not a target: " + volume.health_note) << "\n";
  }
  const Result<std::vector<placement::RelocationProposal>> rows =
      placement::propose_relocations(tools);
  ASSERT_TRUE(rows.has_value()) << rows.error().message;
  for (const placement::RelocationProposal& row : *rows) {
    std::cout << to_string(row.current.kind) << " [" << row.current.setting << "] "
              << utf8(row.current.path) << (row.current.configured ? "" : " (default)") << ", "
              << gb(row.current.size) << " -> "
              << (row.target ? utf8(*row.target) + " (" + std::string(to_string(row.content)) + ")"
                             : row.reason)
              << "\n";
  }

  Win32SystemSpace system;
  Win32ProcessRunner runner;
  if (const Result<placement::HibernationState> state = system.hibernation(); state) {
    if (const std::optional<placement::HibernationMode> mode =
            placement::recommended_hibernation(*state)) {
      print("hibernation", placement::HibernationAction{system, runner, *mode}.summary());
    } else {
      std::cout << "hibernation: " << to_string(state->mode) << ", nothing to recommend\n";
    }
  }
  if (const Result<placement::PageFileState> state = system.page_files(); state) {
    const std::optional<placement::PageFileConfig> config =
        placement::recommended_page_files(*state, *volumes, L"C:\\");
    if (config) {
      print("page file", placement::PageFileAction{system, drives, *config}.summary());
    }
  }
  const Result<std::vector<placement::ShadowStorage>> shadows = system.shadow_storage();
  std::cout << "shadow storage: "
            << (shadows ? std::to_string(shadows->size()) + " volumes" : shadows.error().message)
            << "\n";
}

}  // namespace
}  // namespace myclean::platform
