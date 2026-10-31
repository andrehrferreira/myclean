#include "platform/etw/process_details.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/events/events.hpp"
#include "core/model/units.hpp"

namespace myclean::platform::etw {
namespace {

using namespace std::chrono_literals;

std::string utf8(const wchar_t* text) {
  const int length = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
  result.pop_back();  // the terminator
  return result;
}

events::ProcessKey this_process() {
  FILETIME creation{};
  FILETIME exit{};
  FILETIME kernel{};
  FILETIME user{};
  EXPECT_NE(::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &kernel, &user), FALSE);
  const auto ticks = static_cast<std::int64_t>((std::uint64_t{creation.dwHighDateTime} << 32U) |
                                               creation.dwLowDateTime);
  constexpr std::int64_t unix_epoch = 116'444'736'000'000'000;
  return {.pid = ::GetCurrentProcessId(),
          .created = WallTime{std::chrono::duration_cast<WallTime::duration>(
              std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>{ticks - unix_epoch})}};
}

TEST(CommandLine, OfThisProcess) {
  EXPECT_EQ(command_line_of(this_process()), utf8(::GetCommandLineW()));
}

TEST(CommandLine, NotReadForAnotherInstanceOfThePid) {
  events::ProcessKey reused = this_process();
  reused.created -= 1s;
  EXPECT_FALSE(command_line_of(reused).has_value());
}

TEST(CommandLine, NotReadForAMissingProcess) {
  // PIDs are multiples of 4, so this one never exists.
  EXPECT_FALSE(command_line_of({.pid = 0xFFFFFFF1U, .created = {}}).has_value());
}

TEST(DevicePaths, MapsTheSystemVolumeToItsDriveLetter) {
  std::array<wchar_t, MAX_PATH> system{};
  ASSERT_NE(::GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size())), 0U);
  const std::array<wchar_t, 3> drive = {system[0], L':', L'\0'};
  std::array<wchar_t, MAX_PATH> device{};
  ASSERT_NE(::QueryDosDeviceW(drive.data(), device.data(), static_cast<DWORD>(device.size())), 0U);
  const std::string tail = utf8(system.data()).substr(2);  // "\Windows\system32"

  DevicePaths paths;
  EXPECT_EQ(paths.to_dos(utf8(device.data()) + tail + R"(\cmd.exe)"),
            utf8(system.data()) + R"(\cmd.exe)");
}

TEST(DevicePaths, LeavesOtherPathsUnchanged) {
  DevicePaths paths;
  EXPECT_EQ(paths.to_dos(R"(C:\Windows\notepad.exe)"), R"(C:\Windows\notepad.exe)");
  EXPECT_EQ(paths.to_dos(R"(\Device\NoSuchVolume77\a.exe)"), R"(\Device\NoSuchVolume77\a.exe)");
  EXPECT_EQ(paths.to_dos("explorer.exe"), "explorer.exe");
}

}  // namespace
}  // namespace myclean::platform::etw
