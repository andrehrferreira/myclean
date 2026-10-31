// Smoke tests of the real ETW session. Enabling the kernel providers needs administrator rights
// or SYSTEM; without them the delivery test is skipped with the reason, after checking that the
// failed start left no session behind. Every session these tests create carries the test prefix
// and is stopped by a guard, and stale ones from a crashed run are stopped first.
#include "platform/etw/etw_event_source.hpp"

// clang-format off: evntrace.h needs windows.h first.
#include <windows.h>
#include <evntrace.h>
// clang-format on

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <wil/resource.h>

#include "core/events/events.hpp"
#include "core/model/result.hpp"

namespace myclean::platform::etw {
namespace {

using namespace std::chrono_literals;

constexpr std::wstring_view test_prefix = L"MyClean.Test.E2.";

struct NamedProperties {
  EVENT_TRACE_PROPERTIES properties;
  std::array<wchar_t, 1024> name;
};

NamedProperties properties_for_name() {
  NamedProperties entry{};
  entry.properties.Wnode.BufferSize = sizeof(entry);
  entry.properties.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
  entry.properties.Wnode.ClientContext = 1;
  entry.properties.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
  entry.properties.LoggerNameOffset = offsetof(NamedProperties, name);
  return entry;
}

// Stops the named session when the test ends, however it ends.
class SessionGuard {
 public:
  explicit SessionGuard(std::wstring name) : name_(std::move(name)) {}
  SessionGuard(const SessionGuard&) = delete;
  SessionGuard& operator=(const SessionGuard&) = delete;
  SessionGuard(SessionGuard&&) = delete;
  SessionGuard& operator=(SessionGuard&&) = delete;
  ~SessionGuard() {
    NamedProperties entry = properties_for_name();
    ::ControlTraceW(0, name_.c_str(), &entry.properties, EVENT_TRACE_CONTROL_STOP);
  }

 private:
  std::wstring name_;
};

// The PID of a process that ran and exited.
DWORD exited_pid() {
  STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW)};
  PROCESS_INFORMATION process{};
  std::wstring command = L"cmd.exe /c exit 0";
  if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &startup, &process) == FALSE) {
    return 0;
  }
  const wil::unique_handle handle{process.hProcess};
  const wil::unique_handle thread{process.hThread};
  ::WaitForSingleObject(handle.get(), 10'000);
  return process.dwProcessId;
}

class SessionTest : public ::testing::Test {
 protected:
  void SetUp() override { stop_stale_sessions(test_prefix); }
};

TEST_F(SessionTest, StaleSessionOfAnExitedProcessIsStopped) {
  const DWORD pid = exited_pid();
  ASSERT_NE(pid, 0U);
  const std::wstring name = std::wstring(test_prefix) + std::to_wstring(pid);
  const SessionGuard guard(name);
  NamedProperties entry = properties_for_name();
  TRACEHANDLE handle = 0;
  const ULONG status = ::StartTraceW(&handle, name.c_str(), &entry.properties);
  if (status == ERROR_ACCESS_DENIED) {
    GTEST_SKIP() << "creating an ETW session needs the Performance Log Users group or "
                    "administrator rights";
  }
  ASSERT_EQ(status, static_cast<ULONG>(ERROR_SUCCESS));
  ASSERT_TRUE(session_exists(name));

  // A session of a running process (this one) is left alone.
  const std::wstring mine = std::wstring(test_prefix) + std::to_wstring(::GetCurrentProcessId());
  const SessionGuard mine_guard(mine);
  NamedProperties mine_entry = properties_for_name();
  TRACEHANDLE mine_handle = 0;
  ASSERT_EQ(::StartTraceW(&mine_handle, mine.c_str(), &mine_entry.properties),
            static_cast<ULONG>(ERROR_SUCCESS));

  EXPECT_GE(stop_stale_sessions(test_prefix), 1);
  EXPECT_FALSE(session_exists(name));
  EXPECT_TRUE(session_exists(mine));
}

// Collects the events of one child process.
class ChildSink final : public events::IEventSink {
 public:
  void on_start(const events::ProcessStart& event) override {
    const std::lock_guard lock(mutex_);
    if (event.parent_pid == ::GetCurrentProcessId()) {
      starts_.push_back(event);
      changed_.notify_all();
    }
  }
  void on_stop(const events::ProcessStop& event) override {
    const std::lock_guard lock(mutex_);
    stops_.push_back(event);
    changed_.notify_all();
  }
  void on_network(const events::NetworkTransfer& /*transfer*/) override {}

  // Waits until the start and the stop of `pid` arrived.
  bool wait_for(std::uint32_t pid, events::ProcessStart& start, events::ProcessStop& stop) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 15s, [&] {
      const auto started = std::ranges::find(
          starts_, pid, [](const events::ProcessStart& e) { return e.process.pid; });
      const auto stopped = std::ranges::find(
          stops_, pid, [](const events::ProcessStop& e) { return e.process.pid; });
      if (started == starts_.end() || stopped == stops_.end()) {
        return false;
      }
      start = *started;
      stop = *stopped;
      return true;
    });
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<events::ProcessStart> starts_;
  std::vector<events::ProcessStop> stops_;
};

TEST_F(SessionTest, DeliversAShortLivedProcessOrExplainsWhyNot) {
  ChildSink sink;
  EtwEventSource source{std::wstring(test_prefix)};
  const SessionGuard guard(source.session_name());
  const Result<void> started = source.start(sink);
  if (!started) {
    EXPECT_FALSE(session_exists(source.session_name()));  // the failed start cleaned up
    if (started.error().code == Errc::access_denied) {
      GTEST_SKIP() << "ETW unavailable: " << started.error().message;
    }
    FAIL() << started.error().message;
  }
  EXPECT_TRUE(session_exists(source.session_name()));

  const DWORD child = exited_pid();
  ASSERT_NE(child, 0U);
  events::ProcessStart start;
  events::ProcessStop stop;
  ASSERT_TRUE(sink.wait_for(child, start, stop));
  EXPECT_TRUE(start.image.ends_with(R"(\cmd.exe)")) << start.image;
  EXPECT_EQ(start.image.find(R"(\Device\)"), std::string::npos) << start.image;
  EXPECT_EQ(stop.process, start.process);
  EXPECT_EQ(stop.exit_code, 0U);
  EXPECT_EQ(stop.image, "cmd.exe");

  source.stop();
  EXPECT_FALSE(session_exists(source.session_name()));
}

}  // namespace
}  // namespace myclean::platform::etw
