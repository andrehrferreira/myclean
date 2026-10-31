#include "tray/instance.hpp"

#include <optional>
#include <string>

#include <gtest/gtest.h>
#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"

namespace myclean::tray {
namespace {

// Never the real Local\MyClean.Tray.* names: a tray the owner runs must not notice the tests.
struct Names {
  std::wstring mutex;
  std::wstring event;
};

Names unique_names() {
  static int counter = 0;
  const std::wstring prefix = L"Local\\MyClean.Test." + std::to_wstring(::GetCurrentProcessId()) +
                              L"." + std::to_wstring(++counter);
  return Names{.mutex = prefix + L".Instance", .event = prefix + L".Quit"};
}

std::optional<Instance> acquire(const Names& names) {
  Result<std::optional<Instance>> instance = Instance::acquire(names.mutex, names.event);
  EXPECT_TRUE(instance.has_value());
  return instance ? std::move(*instance) : std::nullopt;
}

TEST(InstanceTest, OnlyTheFirstInstanceRuns) {
  const Names names = unique_names();
  const std::optional<Instance> first = acquire(names);
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(acquire(names)) << "a second instance in the same session must exit";
}

TEST(InstanceTest, TheNextInstanceRunsAfterTheFirstExits) {
  const Names names = unique_names();
  std::optional<Instance> first = acquire(names);
  ASSERT_TRUE(first.has_value());
  first.reset();
  EXPECT_TRUE(acquire(names));
}

TEST(InstanceTest, SignalingTheQuitEventAsksTheTrayToQuit) {
  const Names names = unique_names();
  const std::optional<Instance> instance = acquire(names);
  ASSERT_TRUE(instance.has_value());
  const Instance& tray = *instance;
  EXPECT_FALSE(tray.quit_requested());
  // What deploy.ps1 does: open the existing event and set it.
  const wil::unique_event_nothrow event(
      ::OpenEventW(EVENT_MODIFY_STATE, FALSE, names.event.c_str()));
  ASSERT_TRUE(event);
  event.SetEvent();
  EXPECT_TRUE(tray.quit_requested());
  EXPECT_EQ(::WaitForSingleObject(tray.quit_event(), 0), WAIT_OBJECT_0);
}

TEST(InstanceTest, AQuitLeftSignaledForAPreviousInstanceIsCleared) {
  const Names names = unique_names();
  // Another process still holds the event, signaled for the instance that has exited.
  const wil::unique_event_nothrow stale(::CreateEventW(nullptr, TRUE, TRUE, names.event.c_str()));
  ASSERT_TRUE(stale);
  const std::optional<Instance> instance = acquire(names);
  ASSERT_TRUE(instance.has_value());
  const Instance& tray = *instance;
  EXPECT_FALSE(tray.quit_requested());
}

}  // namespace
}  // namespace myclean::tray
