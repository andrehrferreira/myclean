#include "platform/etw/process_details.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>
#include <vector>

#include <wil/resource.h>
#include <windows.h>
#include <winternl.h>

#include "core/events/events.hpp"
#include "core/model/units.hpp"

namespace myclean::platform::etw {

namespace {

constexpr std::chrono::minutes refresh_interval{1};
constexpr std::string_view device_prefix = R"(\Device\)";
constexpr int process_command_line_information = 60;
constexpr ULONG initial_buffer = 4096;

// FILETIME units: 100 ns since 1601-01-01.
using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
constexpr std::int64_t unix_epoch_ticks = 116'444'736'000'000'000;

std::string utf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length, nullptr, nullptr);
  return result;
}

bool created_at(HANDLE process, WallTime created) {
  FILETIME creation{};
  FILETIME exit{};
  FILETIME kernel{};
  FILETIME user{};
  if (::GetProcessTimes(process, &creation, &exit, &kernel, &user) == FALSE) {
    return false;
  }
  const auto ticks = static_cast<std::int64_t>((std::uint64_t{creation.dwHighDateTime} << 32U) |
                                               creation.dwLowDateTime);
  return std::chrono::duration_cast<Ticks>(created.time_since_epoch()).count() + unix_epoch_ticks ==
         ticks;
}

using QueryFunction = LONG(__stdcall*)(HANDLE, int, void*, ULONG, ULONG*);

QueryFunction query_information_process() {
  const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) {
    return nullptr;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): GetProcAddress returns FARPROC
  return reinterpret_cast<QueryFunction>(::GetProcAddress(ntdll, "NtQueryInformationProcess"));
}

}  // namespace

std::string DevicePaths::to_dos(std::string_view nt_path) {
  if (!nt_path.starts_with(device_prefix)) {
    return std::string(nt_path);
  }
  std::optional<std::string> path = mapped(nt_path);
  const auto now = std::chrono::steady_clock::now();
  if (!path && (!refreshed_ || now - *refreshed_ >= refresh_interval)) {
    refresh();
    refreshed_ = now;
    path = mapped(nt_path);
  }
  return path ? *path : std::string(nt_path);
}

std::optional<std::string> DevicePaths::mapped(std::string_view nt_path) const {
  for (const auto& [device, drive] : devices_) {
    if (nt_path.starts_with(device) &&
        (nt_path.size() == device.size() || nt_path[device.size()] == '\\')) {
      return drive + std::string(nt_path.substr(device.size()));
    }
  }
  return std::nullopt;
}

void DevicePaths::refresh() {
  devices_.clear();
  const DWORD drives = ::GetLogicalDrives();
  for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
    if ((drives & (1U << static_cast<unsigned>(letter - L'A'))) == 0) {
      continue;
    }
    const std::array<wchar_t, 3> drive = {letter, L':', L'\0'};
    std::array<wchar_t, MAX_PATH> target{};
    if (::QueryDosDeviceW(drive.data(), target.data(), static_cast<DWORD>(target.size())) != 0) {
      devices_.emplace_back(utf8(target.data()), utf8(drive.data()));
    }
  }
}

std::optional<std::string> command_line_of(const events::ProcessKey& process) {
  static const auto query = query_information_process();
  if (query == nullptr) {
    return std::nullopt;
  }
  const wil::unique_handle handle{
      ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.pid)};
  if (!handle || !created_at(handle.get(), process.created)) {
    return std::nullopt;
  }
  // The answer is a UNICODE_STRING followed by its text.
  std::vector<std::byte> buffer(initial_buffer);
  ULONG needed = 0;
  LONG status = query(handle.get(), process_command_line_information, buffer.data(),
                      static_cast<ULONG>(buffer.size()), &needed);
  if (status < 0 && needed > buffer.size()) {
    buffer.resize(needed);
    status = query(handle.get(), process_command_line_information, buffer.data(),
                   static_cast<ULONG>(buffer.size()), &needed);
  }
  if (status < 0 || buffer.size() < sizeof(UNICODE_STRING)) {
    return std::nullopt;
  }
  UNICODE_STRING text{};
  std::memcpy(&text, buffer.data(), sizeof(text));
  return utf8(std::wstring_view{text.Buffer, text.Length / sizeof(wchar_t)});
}

}  // namespace myclean::platform::etw
