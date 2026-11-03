#include "tray/uninstall_launcher.hpp"

#include <chrono>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>

#include <wil/resource.h>
#include <windows.h>
// Included after windows.h, which they need.
#include <objbase.h>
#include <shellapi.h>

#include "core/model/result.hpp"

namespace myclean::tray {

namespace {

constexpr DWORD poll_ms = 500;

std::wstring widen(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int size =
      ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

std::string narrow(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                        nullptr, nullptr);
  return result;
}

}  // namespace

Result<void> Win32UninstallLauncher::launch(const std::filesystem::path& program,
                                            std::string_view arguments, std::chrono::minutes wait,
                                            const std::stop_token& stop) {
  if (program.empty()) {
    return fail(Errc::invalid_argument, "no program to start");  // the shell would open a folder
  }
  // ShellExecuteEx may hand the file to a shell extension, which needs COM on this thread.
  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const auto uninitialize = wil::scope_exit([com] {
    if (SUCCEEDED(com)) {
      ::CoUninitialize();
    }
  });
  const std::wstring& file = program.native();
  const std::wstring parameters = widen(arguments);
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"open";
  info.lpFile = file.c_str();
  info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
  info.nShow = show_ ? SW_SHOWNORMAL : SW_HIDE;
  if (::ShellExecuteExW(&info) == FALSE) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_CANCELLED) {
      return fail(Errc::refused, "the elevation prompt was declined");
    }
    const Errc code = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                          ? Errc::not_found
                          : Errc::io_error;
    return fail(code,
                "cannot start " + narrow(file) + " (Windows error " + std::to_string(error) + ")");
  }
  const wil::unique_process_handle process(info.hProcess);
  if (!process) {
    return {};  // handed to a running program: there is no process to wait for
  }
  const auto deadline = std::chrono::steady_clock::now() + wait;
  while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
    if (::WaitForSingleObject(process.get(), poll_ms) != WAIT_TIMEOUT) {
      break;
    }
  }
  return {};
}

}  // namespace myclean::tray
