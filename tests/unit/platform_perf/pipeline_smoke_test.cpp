#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
// clang-format off
#include <windows.h>  // first: psapi.h needs its types
#include <psapi.h>
// clang-format on

#include "core/model/result.hpp"
#include "core/os/clock.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/gpu_collector.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/telemetry/pipeline.hpp"
#include "core/telemetry/process_collector.hpp"
#include "core/telemetry/registration.hpp"
#include "core/telemetry/system_collectors.hpp"
#include "core/telemetry/volume_collector.hpp"
#include "platform/disk/win32_volume_info.hpp"
#include "platform/perf/pdh_counters.hpp"
#include "platform/perf/win32_memory_status.hpp"
#include "platform/proc/nt_process_table.hpp"

namespace myclean::platform {
namespace {

// How long the pipeline runs: MYCLEAN_TELEMETRY_SMOKE_SECONDS, 5 s by default. A longer run
// gives a steadier overhead figure.
std::chrono::seconds run_time() {
  std::array<wchar_t, 16> text{};
  const DWORD length = ::GetEnvironmentVariableW(L"MYCLEAN_TELEMETRY_SMOKE_SECONDS", text.data(),
                                                 static_cast<DWORD>(text.size()));
  if (length == 0 || length >= text.size()) {
    return std::chrono::seconds{5};
  }
  return std::chrono::seconds{std::stoi(std::wstring(text.data(), length))};
}

std::chrono::nanoseconds process_cpu_time() {
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  ::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user);
  const auto ticks = [](const FILETIME& time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
  };
  return std::chrono::nanoseconds{(ticks(kernel) + ticks(user)) * 100};
}

double private_megabytes() {
  PROCESS_MEMORY_COUNTERS_EX counters{.cb = sizeof(PROCESS_MEMORY_COUNTERS_EX)};
  ::GetProcessMemoryInfo(::GetCurrentProcess(),
                         // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Win32 API
                         reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
  return static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0);
}

TEST(TelemetryPipeline, EveryCollectorRunsOnThisMachineAndReportsItsOverhead) {
  const std::filesystem::path folder =
      std::filesystem::temp_directory_path() / "myclean-telemetry-pipeline-smoke";
  std::filesystem::remove_all(folder);
  std::filesystem::create_directories(folder);
  const os::SystemClock clock;
  {
    Result<std::unique_ptr<store::Store>> store = store::Store::open(folder / "myclean.db", clock);
    ASSERT_TRUE(store.has_value()) << store.error().message;

    NtProcessTable processes;
    PdhCounters counters;
    const Win32MemoryStatus memory;
    const Win32VolumeInfo volumes;
    telemetry::Pipeline pipeline{clock, **store};
    const double memory_before = private_megabytes();
    const telemetry::Registration registration =
        telemetry::register_collectors(pipeline, processes, counters, memory, volumes);
    for (const Error& error : registration.unavailable) {
      std::cout << "unavailable: " << error.message << "\n";
      EXPECT_TRUE(error.message.starts_with("gpu: ")) << error.message;
    }

    const std::chrono::seconds duration = run_time();
    const std::chrono::nanoseconds cpu_before = process_cpu_time();
    const auto start = std::chrono::steady_clock::now();
    int polls = 0;
    while (std::chrono::steady_clock::now() - start < duration) {
      for (const Error& error : pipeline.poll()) {
        ADD_FAILURE() << error.message;
      }
      ++polls;
      std::this_thread::sleep_until(*pipeline.next_due());
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    const auto cpu = std::chrono::duration<double>(process_cpu_time() - cpu_before);

    const std::vector<Sample> total =
        (*store)->raw({.metric = std::string(telemetry::metric::cpu_utility),
                       .entity = std::string(telemetry::metric::system)});
    // The first reading of a rate counter has no value, and a sample is skipped when a poll
    // overruns its second on a busy machine (the debug build is several times slower).
    EXPECT_GE(total.size() * 10, static_cast<std::size_t>(duration.count()) * 7);
    ASSERT_NE(registration.live.processes->latest(), nullptr);
    EXPECT_GT(registration.live.processes->latest()->trees.size(), 5U);
    ASSERT_NE(registration.live.volumes->latest(), nullptr);
    EXPECT_FALSE(registration.live.volumes->latest()->volumes.empty());

    const double processors = std::thread::hardware_concurrency();
    const double machine_percent = 100.0 * cpu.count() / elapsed.count() / processors;
    const double private_mb = private_megabytes();
    std::cout << "polls: " << polls << " in " << elapsed.count() << " s\n"
              << "collector CPU: " << 1000.0 * cpu.count() / elapsed.count()
              << " ms per second = " << 100.0 * cpu.count() / elapsed.count()
              << " % of one logical processor = " << machine_percent << " % of the machine\n"
              << "private memory: " << private_mb << " MB (+" << private_mb - memory_before
              << " MB since the collectors opened)\n";
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    // The build that ships is held to the budget. The debug build runs unoptimized under
    // AddressSanitizer: several times the CPU, and its shadow memory and the quarantine of freed
    // blocks (up to 256 MB by default) count as private bytes, so its figures are only reported.
    // NFR-PERF-1: the service and the tray together stay under 1 % of the machine's CPU and
    // 150 MB of RAM. The collectors are the service's steady work, so they alone must fit.
    constexpr double cpu_budget_percent = 1.0;
    constexpr double memory_budget_mb = 150.0;
    EXPECT_LT(machine_percent, cpu_budget_percent);
    EXPECT_LT(private_mb - memory_before, memory_budget_mb);
    EXPECT_LT(private_mb, memory_budget_mb);
#endif
    ASSERT_TRUE((*store)->close().has_value());
  }
  std::error_code ignored;
  std::filesystem::remove_all(folder, ignored);
}

// CPU time, not duration: the machine may be busy. Thread and process times advance in 15.6 ms
// ticks, so the cost is averaged over several samples.
TEST(TelemetryPipeline, EachCollectorReportsItsCostPerSample) {
  NtProcessTable processes;
  PdhCounters counters;
  const Win32MemoryStatus memory;
  const Win32VolumeInfo volumes;
  std::vector<std::unique_ptr<telemetry::ICollector>> collectors;
  collectors.push_back(std::make_unique<telemetry::ProcessCollector>(processes));
  collectors.push_back(*telemetry::make_cpu_collector(counters));
  collectors.push_back(*telemetry::make_memory_collector(memory, counters));
  collectors.push_back(*telemetry::make_disk_collector(counters));
  if (Result<std::unique_ptr<telemetry::GpuCollector>> gpu =
          telemetry::GpuCollector::open(counters)) {
    collectors.push_back(std::move(*gpu));
  }
  collectors.push_back(std::make_unique<telemetry::VolumeCollector>(volumes));

  constexpr int samples = 10;
  for (const std::unique_ptr<telemetry::ICollector>& collector : collectors) {
    // The first sample loads the counter providers; steady state is what matters.
    ASSERT_TRUE(
        collector->collect(std::chrono::system_clock::now(), std::chrono::steady_clock::now()));
    std::chrono::nanoseconds cost{};
    for (int sample = 0; sample < samples; ++sample) {
      const std::chrono::nanoseconds before = process_cpu_time();
      const Result<std::vector<telemetry::Measurement>> measured =
          collector->collect(std::chrono::system_clock::now(), std::chrono::steady_clock::now());
      cost += process_cpu_time() - before;
      ASSERT_TRUE(measured.has_value()) << collector->name() << ": " << measured.error().message;
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    std::cout << collector->name() << ": "
              << std::chrono::duration<double, std::milli>(cost).count() / samples
              << " ms CPU per sample\n";
  }
}

}  // namespace
}  // namespace myclean::platform
