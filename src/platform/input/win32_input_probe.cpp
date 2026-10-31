#include "platform/input/win32_input_probe.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// clang-format off
#include <windows.h>  // first: tlhelp32.h needs its types
#include <tlhelp32.h>
// clang-format on
#include <wil/resource.h>

#include "core/input/incident.hpp"
#include "core/input/probe.hpp"

namespace myclean::platform {

namespace {

constexpr UINT first_registered_format = 0xC000;

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

// The image file name of `pid`, empty when the process cannot be opened.
std::string image_of(DWORD pid) {
  const wil::unique_handle process{::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
  if (!process) {
    return {};
  }
  std::wstring path(UNICODE_STRING_MAX_CHARS, L'\0');
  auto size = static_cast<DWORD>(path.size());
  if (::QueryFullProcessImageNameW(process.get(), 0, path.data(), &size) == FALSE) {
    return {};
  }
  path.resize(size);
  return utf8(std::filesystem::path(path).filename().native());
}

std::optional<input::WindowRef> describe(HWND window) {
  if (window == nullptr) {
    return std::nullopt;
  }
  DWORD pid = 0;
  const DWORD thread = ::GetWindowThreadProcessId(window, &pid);
  if (thread == 0) {
    return std::nullopt;  // the window is gone
  }
  return input::WindowRef{.handle = std::bit_cast<std::uintptr_t>(window),
                          .thread = thread,
                          .process = {.pid = pid, .image = image_of(pid)}};
}

std::uint64_t ticks(const FILETIME& time) {
  return (std::uint64_t{time.dwHighDateTime} << 32U) | time.dwLowDateTime;
}

BOOL CALLBACK collect_window(HWND window, LPARAM list) {
  std::bit_cast<std::vector<HWND>*>(list)->push_back(window);
  return TRUE;
}

}  // namespace

std::optional<input::WindowRef> Win32InputProbe::foreground() {
  return describe(::GetForegroundWindow());
}

std::optional<input::WindowRef> Win32InputProbe::clipboard_owner() {
  return describe(::GetClipboardOwner());
}

std::optional<input::WindowRef> Win32InputProbe::open_clipboard_window() {
  return describe(::GetOpenClipboardWindow());
}

std::optional<input::WindowRef> Win32InputProbe::clipboard_viewer() {
  return describe(::GetClipboardViewer());
}

bool Win32InputProbe::answers(const input::WindowRef& window, std::chrono::milliseconds timeout) {
  DWORD_PTR result = 0;
  return ::SendMessageTimeoutW(std::bit_cast<HWND>(static_cast<std::uintptr_t>(window.handle)),
                               WM_NULL, 0, 0, SMTO_NORMAL, static_cast<UINT>(timeout.count()),
                               &result) != 0;
}

std::vector<input::ClipboardFormat> Win32InputProbe::formats() {
  std::vector<UINT> ids(32);
  UINT count = 0;
  while (::GetUpdatedClipboardFormats(ids.data(), static_cast<UINT>(ids.size()), &count) == FALSE) {
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      return {};
    }
    ids.resize(std::max<std::size_t>(count, ids.size() * 2));
  }
  std::vector<input::ClipboardFormat> formats;
  for (const UINT id : std::span(ids).first(count)) {
    input::ClipboardFormat format{.id = id};
    if (id >= first_registered_format) {
      std::array<wchar_t, 256> name{};
      const int length = ::GetClipboardFormatNameW(id, name.data(), static_cast<int>(name.size()));
      format.name = utf8(std::wstring_view(name.data(), static_cast<std::size_t>(length)));
    }
    formats.push_back(std::move(format));
  }
  return formats;
}

std::vector<input::ProcessRef> Win32InputProbe::hung_windows() {
  // NOLINTNEXTLINE(misc-const-correctness): filled by collect_window through EnumWindows
  std::vector<HWND> windows;
  ::EnumWindows(collect_window, std::bit_cast<LPARAM>(&windows));
  std::vector<input::ProcessRef> hung;
  for (HWND window : windows) {
    if (::IsWindowVisible(window) == FALSE || ::IsHungAppWindow(window) == FALSE) {
      continue;
    }
    const std::optional<input::WindowRef> described = describe(window);
    if (described && std::ranges::find(hung, described->process) == hung.end()) {
      hung.push_back(described->process);
    }
  }
  return hung;
}

std::vector<std::string> Win32InputProbe::running_images() {
  const wil::unique_hfile snapshot{::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
  if (!snapshot) {
    return {};
  }
  std::vector<std::string> images;
  PROCESSENTRY32W entry{.dwSize = sizeof(PROCESSENTRY32W)};
  for (BOOL found = ::Process32FirstW(snapshot.get(), &entry); found != FALSE;
       found = ::Process32NextW(snapshot.get(), &entry)) {
    images.push_back(utf8(std::wstring_view(std::data(entry.szExeFile))));
  }
  return images;
}

std::optional<input::CpuTimes> Win32InputProbe::cpu_times() {
  FILETIME idle{};
  FILETIME kernel{};
  FILETIME user{};
  if (::GetSystemTimes(&idle, &kernel, &user) == FALSE) {
    return std::nullopt;
  }
  // Kernel time includes idle time.
  return input::CpuTimes{.idle = ticks(idle), .busy = ticks(kernel) + ticks(user) - ticks(idle)};
}

std::optional<int> Win32InputProbe::memory_load_percent() {
  MEMORYSTATUSEX status{.dwLength = sizeof(MEMORYSTATUSEX)};
  if (::GlobalMemoryStatusEx(&status) == FALSE) {
    return std::nullopt;
  }
  return static_cast<int>(status.dwMemoryLoad);
}

}  // namespace myclean::platform
