// Win32ProcessControl on the workload helper only: every setting is read back and restored.

#include "core/os/process_control.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "jobs_test_support.hpp"
#include "platform/jobs/win32_process_control.hpp"
#include "workload_process.hpp"

namespace myclean::platform::testing {
namespace {

using namespace std::chrono_literals;

using ProcessControlTest = JobsTest;

// True when the process gets no CPU time over `window`.
bool frozen(const Workload& workload, std::chrono::milliseconds window) {
  const std::chrono::nanoseconds before = workload.cpu_time();
  std::this_thread::sleep_for(window);
  return workload.cpu_time() == before;
}

// True when the process gets CPU time within `timeout` (it may wait behind other work).
bool runs(const Workload& workload, std::chrono::milliseconds timeout) {
  const std::chrono::nanoseconds before = workload.cpu_time();
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(50ms);
    if (workload.cpu_time() > before) {
      return true;
    }
  }
  return false;
}

TEST_F(ProcessControlTest, PrioritySetAndRestored) {
  const Workload workload = Workload::start(L"exit 30000 0");
  ASSERT_TRUE(workload.started()) << workload.error();
  Win32ProcessControl control;

  Result<os::PriorityClass> current = control.priority(workload.key());
  ASSERT_TRUE(current) << current.error().message;
  EXPECT_EQ(*current, os::PriorityClass::below_normal);

  const Result<os::PriorityClass> previous =
      control.set_priority(workload.key(), os::PriorityClass::idle);
  ASSERT_TRUE(previous) << previous.error().message;
  EXPECT_EQ(*previous, os::PriorityClass::below_normal);
  current = control.priority(workload.key());
  ASSERT_TRUE(current);
  EXPECT_EQ(*current, os::PriorityClass::idle);

  ASSERT_TRUE(control.set_priority(workload.key(), *previous));
  current = control.priority(workload.key());
  ASSERT_TRUE(current);
  EXPECT_EQ(*current, os::PriorityClass::below_normal);
}

TEST_F(ProcessControlTest, MemoryPrioritySetAndRestored) {
  const Workload workload = Workload::start(L"exit 30000 0");
  ASSERT_TRUE(workload.started()) << workload.error();
  Win32ProcessControl control;

  const Result<os::MemoryPriority> original = control.memory_priority(workload.key());
  ASSERT_TRUE(original) << original.error().message;

  const Result<os::MemoryPriority> previous =
      control.set_memory_priority(workload.key(), os::MemoryPriority::low);
  ASSERT_TRUE(previous) << previous.error().message;
  EXPECT_EQ(*previous, *original);
  Result<os::MemoryPriority> current = control.memory_priority(workload.key());
  ASSERT_TRUE(current);
  EXPECT_EQ(*current, os::MemoryPriority::low);

  ASSERT_TRUE(control.set_memory_priority(workload.key(), *previous));
  current = control.memory_priority(workload.key());
  ASSERT_TRUE(current);
  EXPECT_EQ(*current, *original);
}

TEST_F(ProcessControlTest, EcoQosSetAndRestored) {
  const Workload workload = Workload::start(L"exit 30000 0");
  ASSERT_TRUE(workload.started()) << workload.error();
  Win32ProcessControl control;

  Result<os::EcoQos> current = control.eco_qos(workload.key());
  ASSERT_TRUE(current) << current.error().message;
  EXPECT_EQ(*current, os::EcoQos::automatic);

  const Result<os::EcoQos> previous = control.set_eco_qos(workload.key(), os::EcoQos::on);
  ASSERT_TRUE(previous) << previous.error().message;
  EXPECT_EQ(*previous, os::EcoQos::automatic);
  current = control.eco_qos(workload.key());
  ASSERT_TRUE(current);
  EXPECT_EQ(*current, os::EcoQos::on);

  ASSERT_TRUE(control.set_eco_qos(workload.key(), os::EcoQos::off));
  current = control.eco_qos(workload.key());
  ASSERT_TRUE(current);
  EXPECT_EQ(*current, os::EcoQos::off);

  const Result<os::EcoQos> restored = control.set_eco_qos(workload.key(), *previous);
  ASSERT_TRUE(restored);
  EXPECT_EQ(*restored, os::EcoQos::off);
  current = control.eco_qos(workload.key());
  ASSERT_TRUE(current);
  EXPECT_EQ(*current, os::EcoQos::automatic);
}

TEST_F(ProcessControlTest, SuspendsAreCounted) {
  const Workload workload = Workload::start(L"spin 1 30000");
  ASSERT_TRUE(workload.started()) << workload.error();
  Win32ProcessControl control;
  ASSERT_TRUE(runs(workload, 5s)) << "the workload never got the CPU";

  ASSERT_TRUE(control.suspend(workload.key()));
  ASSERT_TRUE(control.suspend(workload.key()));
  std::this_thread::sleep_for(50ms);  // a running quantum may still be accounted
  EXPECT_TRUE(frozen(workload, 300ms));

  ASSERT_TRUE(control.resume(workload.key()));
  EXPECT_TRUE(frozen(workload, 300ms)) << "one resume must not undo two suspends";

  ASSERT_TRUE(control.resume(workload.key()));
  EXPECT_TRUE(runs(workload, 5s));
}

TEST_F(ProcessControlTest, TerminatesWithAnExitCode) {
  const Workload workload = Workload::start(L"exit 30000 0");
  ASSERT_TRUE(workload.started()) << workload.error();
  Win32ProcessControl control;

  ASSERT_TRUE(control.terminate(workload.key(), 42));
  EXPECT_EQ(workload.wait(10s), 42U);

  const Result<void> again = control.terminate(workload.key(), 42);
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error().code, Errc::not_found) << again.error().message;
}

TEST_F(ProcessControlTest, RefusesAReusedPid) {
  const Workload workload = Workload::start(L"exit 30000 0");
  ASSERT_TRUE(workload.started()) << workload.error();
  Win32ProcessControl control;
  const os::ProcessKey older{.pid = workload.key().pid,
                             .created = workload.key().created - WallTime::duration{1}};

  const Result<os::PriorityClass> priority = control.set_priority(older, os::PriorityClass::idle);
  ASSERT_FALSE(priority);
  EXPECT_EQ(priority.error().code, Errc::not_found) << priority.error().message;
  EXPECT_FALSE(control.suspend(older));
  EXPECT_FALSE(control.terminate(older, 1));
  EXPECT_FALSE(workload.wait(0ms).has_value()) << "the real process must be untouched";
  const Result<os::PriorityClass> unchanged = control.priority(workload.key());
  ASSERT_TRUE(unchanged);
  EXPECT_EQ(*unchanged, os::PriorityClass::below_normal);
}

TEST(ProcessKey, OfALiveAndAMissingProcess) {
  const Result<os::ProcessKey> self = process_key(::GetCurrentProcessId());
  ASSERT_TRUE(self) << self.error().message;
  EXPECT_EQ(self->pid, ::GetCurrentProcessId());
  EXPECT_LT(self->created, std::chrono::system_clock::now());

  const Result<os::ProcessKey> missing = process_key(0xFFFFFFF0U);
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().code, Errc::not_found);
}

}  // namespace
}  // namespace myclean::platform::testing
