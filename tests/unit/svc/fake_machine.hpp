#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/telemetry/memory_status.hpp"
#include "core/telemetry/process_table.hpp"
#include "core/telemetry/volume_info.hpp"
#include "platform/ipc/security.hpp"
#include "svc/component.hpp"
#include "svc/service.hpp"
#include "svc/telemetry_component.hpp"
#include "telemetry/fake_sources.hpp"

// A small fake machine for the telemetry component: 32 logical processors, 64 GiB of memory,
// disks 2 and 10, volume C: on disk 2, no GPU counters, and these process trees:
//   pid 0    System Idle Process  (idle time; must never be listed)
//   pid 100  browser.exe          with child pid 101; 9 % CPU of the machine once rates exist
//   pid 200  editor.exe           8 GiB private working set (12.5 % of memory)
//   pid 300  small.exe            nothing to speak of
namespace myclean::svc::testing {

using namespace myclean::literals;

// Volumes a test can change or make fail after the component owns them. Not thread-safe:
// change it only while nothing samples.
class FakeVolumes final : public telemetry::IVolumeInfo {
 public:
  [[nodiscard]] Result<std::vector<telemetry::VolumeStatus>> volumes() const override {
    if (failure_) {
      return fail(Errc::io_error, *failure_);
    }
    return volumes_;
  }

  std::vector<telemetry::VolumeStatus>& list() { return volumes_; }
  void fail_with(std::string message) { failure_ = std::move(message); }

 private:
  std::vector<telemetry::VolumeStatus> volumes_{telemetry::VolumeStatus{.id = R"(\\?\Volume{c}\)",
                                                                        .mount_points = {R"(C:\)"},
                                                                        .size = 1000_GiB,
                                                                        .free = 100_GiB,
                                                                        .disks = {2}}};
  std::optional<std::string> failure_;
};

struct FakeMachine {
  telemetry::testing::FakeProcessTable* processes = nullptr;
  telemetry::testing::FakeCounters* counters = nullptr;
  FakeVolumes* volumes = nullptr;
  TelemetrySources sources;
};

inline const WallTime fake_boot = std::chrono::sys_days{std::chrono::October / 30 / 2026};

inline telemetry::ProcessInfo fake_process(std::uint32_t pid, std::uint32_t parent,
                                           std::string image, Bytes private_working_set) {
  return telemetry::ProcessInfo{.pid = pid,
                                .parent_pid = parent,
                                .created = fake_boot + std::chrono::seconds{pid},
                                .image = std::move(image),
                                .private_bytes = private_working_set,
                                .working_set = private_working_set,
                                .private_working_set = private_working_set,
                                .handles = 10,
                                .threads = 2};
}

inline FakeMachine fake_machine() {
  auto processes = std::make_unique<telemetry::testing::FakeProcessTable>(32);
  processes->processes() = {
      fake_process(0, 0, "System Idle Process", 8_KiB),
      fake_process(100, 1, "browser.exe", 300_MiB), fake_process(101, 100, "browser.exe", 200_MiB),
      fake_process(200, 1, "editor.exe", 8_GiB), fake_process(300, 1, "small.exe", 10_MiB)};
  auto counters = std::make_unique<telemetry::testing::FakeCounters>();
  counters->set(R"(\Processor Information(*)\% Processor Utility)",
                {{.instance = "0,0", .value = 30.0}, {.instance = "_Total", .value = 12.5}});
  counters->set(R"(\Processor Information(_Total)\% DPC Time)", 0.25);
  counters->set(R"(\PhysicalDisk(*)\Current Disk Queue Length)",
                {{.instance = "10 E:", .value = 1.0}, {.instance = "2 C:", .value = 2.0}});
  counters->set(R"(\PhysicalDisk(*)\Disk Read Bytes/sec)", {{.instance = "2 C:", .value = 4096.0}});
  counters->remove(R"(\GPU Engine(*)\Utilization Percentage)");
  auto volumes = std::make_unique<FakeVolumes>();
  FakeMachine machine{.processes = processes.get(),
                      .counters = counters.get(),
                      .volumes = volumes.get(),
                      .sources = {}};
  machine.sources = TelemetrySources{
      .processes = std::move(processes),
      .counters = std::move(counters),
      .memory = std::make_unique<telemetry::testing::FakeMemoryStatus>(telemetry::MemoryStatus{
          .total = 64_GiB, .available = 32_GiB, .commit = 40_GiB, .commit_limit = 80_GiB}),
      .volumes = std::move(volumes),
      .system_drive = "C:"};
  return machine;
}

// Gives the browser tree 9 % of 32 processors over `elapsed` (2.88 processor-seconds per second,
// split between its two processes) and the idle process 50 %.
inline void burn_browser_cpu(telemetry::testing::FakeProcessTable& table,
                             std::chrono::seconds elapsed) {
  const auto share = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed) * 144 / 100;
  for (telemetry::ProcessInfo& process : table.processes()) {
    if (process.pid == 100 || process.pid == 101) {
      process.cpu_time += share;
    } else if (process.pid == 0) {
      process.cpu_time += std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed) * 16;
    }
  }
}

// A console-mode service in this process with only the telemetry component over `machine`,
// its store in `dir` and its pipe at `pipe`.
inline Result<std::unique_ptr<Service>> start_telemetry_service(FakeMachine machine,
                                                                const std::filesystem::path& dir,
                                                                const std::wstring& pipe,
                                                                const os::IClock& clock) {
  Result<std::string> owner = platform::current_user_sid();
  if (!owner) {
    return tl::unexpected(owner.error());
  }
  std::vector<std::unique_ptr<IComponent>> components;
  components.push_back(std::make_unique<TelemetryComponent>(std::move(machine.sources)));
  return Service::start(ServiceOptions{.database = dir / "myclean.db",
                                       .data_dir = dir / "data",
                                       .pipe_name = pipe,
                                       .owner_sid = std::move(*owner),
                                       .mode = "console",
                                       .version = std::nullopt},
                        std::move(components), clock);
}

}  // namespace myclean::svc::testing
