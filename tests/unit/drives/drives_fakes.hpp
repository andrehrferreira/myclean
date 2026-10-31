#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/drives/actions.hpp"
#include "core/drives/analysis.hpp"
#include "core/drives/history.hpp"
#include "core/drives/layout.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/settings.hpp"
#include "core/space/scanner.hpp"

namespace myclean::drives::testing {

inline space::ListedEntry file(const std::string& name, std::uint64_t size_on_disk) {
  return space::ListedEntry{
      .name = name, .size = Bytes{size_on_disk}, .size_on_disk = Bytes{size_on_disk}};
}

inline space::ListedEntry folder(const std::string& name) {
  return space::ListedEntry{.name = name, .directory = true};
}

// An in-memory directory tree; folders without a listing fail with not_found.
class FakeReader final : public space::IDirectoryReader {
 public:
  std::map<std::filesystem::path, std::vector<space::ListedEntry>> listings;
  std::function<void()> on_list;

  [[nodiscard]] Result<std::vector<space::ListedEntry>> list(
      const std::filesystem::path& path) const override {
    if (on_list) {
      on_list();
    }
    const auto found = listings.find(path);
    if (found == listings.end()) {
      return fail(Errc::not_found, "no such folder");
    }
    return found->second;
  }
};

// Cluster runs per file (a file without runs fails with access_denied) and one free-space result.
class FakeLayout final : public IVolumeLayout {
 public:
  std::map<std::filesystem::path, std::vector<Extent>> runs;
  Result<FreeSpaceLayout> free = fail(Errc::access_denied, "opening the volume: Win32 error 5");
  mutable std::vector<std::filesystem::path> bitmap_reads;

  [[nodiscard]] Result<std::vector<Extent>> extents(
      const std::filesystem::path& path) const override {
    const auto found = runs.find(path);
    if (found == runs.end()) {
      return fail(Errc::access_denied, "opening: Win32 error 5");
    }
    return found->second;
  }
  [[nodiscard]] Result<FreeSpaceLayout> free_space(
      const std::filesystem::path& root, std::chrono::milliseconds /*budget*/) const override {
    bitmap_reads.push_back(root);
    return free;
  }
};

// `count` runs of `clusters` each with a gap between them: `count` fragments.
inline std::vector<Extent> scattered(std::uint64_t count, std::uint64_t clusters = 16) {
  std::vector<Extent> runs;
  runs.reserve(count);
  for (std::uint64_t index = 0; index < count; ++index) {
    runs.push_back(
        Extent{.lcn = static_cast<std::int64_t>(index * clusters * 2), .clusters = clusters});
  }
  return runs;
}

class FakeLog final : public IOptimizerLog {
 public:
  Result<std::vector<OptimizerEvent>> logged = std::vector<OptimizerEvent>{};

  [[nodiscard]] Result<std::vector<OptimizerEvent>> events() const override { return logged; }
};

// Event 258 as Windows 11 26200 logs it, with the binary data observed for each kind.
inline OptimizerEvent optimized(WallTime time, std::string volume, OptimizationKind kind) {
  std::vector<std::uint8_t> data = {0, 0, 0, 0, 0xED, 0x01, 0, 0, 0xA3, 0x01, 0, 0};
  std::string operation = "desfragmentação";
  if (kind == OptimizationKind::retrim) {
    data = {0, 0, 0, 0, 0xBC, 0x02, 0, 0, 0x84, 0x02, 0, 0};
    operation = "otimizar novamente";
  }
  return OptimizerEvent{.time = time,
                        .id = event_optimized,
                        .operation = std::move(operation),
                        .volume = std::move(volume),
                        .data = std::move(data)};
}

class FakeSeekPenalty final : public placement::ISeekPenalty {
 public:
  std::map<std::uint32_t, bool> penalty;

  [[nodiscard]] std::optional<bool> seek_penalty(std::uint32_t disk) const override {
    const auto found = penalty.find(disk);
    return found == penalty.end() ? std::nullopt : std::optional{found->second};
  }
};

class FakeTrim final : public ITrimSupport {
 public:
  std::map<std::uint32_t, bool> supported;
  std::optional<bool> ntfs = true;

  [[nodiscard]] std::optional<bool> trim_supported(std::uint32_t disk) const override {
    const auto found = supported.find(disk);
    return found == supported.end() ? std::nullopt : std::optional{found->second};
  }
  [[nodiscard]] std::optional<bool> ntfs_trim_enabled() const override { return ntfs; }
};

class FakeDriveStates final : public IDriveStates {
 public:
  std::vector<DriveState> states;

  [[nodiscard]] Result<std::vector<DriveState>> drives() const override { return states; }
};

class FakeIdle final : public IDiskIdle {
 public:
  std::map<std::uint32_t, std::optional<double>> percent;

  [[nodiscard]] Result<std::optional<double>> idle_percent(
      std::uint32_t disk, std::chrono::seconds /*window*/) const override {
    const auto found = percent.find(disk);
    return found == percent.end() ? std::optional<double>{} : found->second;
  }
};

class FakePower final : public IPowerSource {
 public:
  bool battery = false;

  [[nodiscard]] Result<bool> on_battery() const override { return battery; }
};

// Records every command; answers with `exit_code`.
class FakeRunner final : public placement::IProcessRunner {
 public:
  std::vector<placement::Command> ran;
  std::uint32_t exit_code = 0;

  [[nodiscard]] Result<placement::CommandResult> run(const placement::Command& command) override {
    ran.push_back(command);
    return placement::CommandResult{.exit_code = exit_code, .output = "done"};
  }
};

inline DriveState ssd(std::filesystem::path root, std::uint32_t disk) {
  return DriveState{.root = std::move(root),
                    .disks = {disk},
                    .drive_class = placement::DriveClass::nvme,
                    .media = Media::ssd,
                    .healthy = true,
                    .trim_supported = true,
                    .ntfs_trim_enabled = true};
}

inline DriveState hdd(std::filesystem::path root, std::uint32_t disk) {
  return DriveState{.root = std::move(root),
                    .disks = {disk},
                    .drive_class = placement::DriveClass::hdd,
                    .media = Media::hdd,
                    .healthy = true};
}

}  // namespace myclean::drives::testing
