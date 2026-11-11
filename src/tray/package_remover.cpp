#include "tray/package_remover.hpp"

#include <chrono>
#include <cstdint>
#include <format>
#include <stop_token>
#include <string>
#include <thread>

// clang-format off
#include <windows.h>  // first: the WinRT headers need its types
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>
// clang-format on

#include "core/model/result.hpp"

namespace myclean::tray {

namespace {

using winrt::Windows::Management::Deployment::PackageManager;

void enter_apartment() {
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
  } catch (const winrt::hresult_error&) {  // NOLINT(bugprone-empty-catch): a thread's other mode
  }
}

tl::unexpected<Error> failed(std::string_view what, const winrt::hresult_error& error) {
  return fail(
      error.code() == E_ACCESSDENIED ? Errc::access_denied : Errc::io_error,
      std::format("{} failed (0x{:08x}): {}", what, static_cast<std::uint32_t>(error.code().value),
                  winrt::to_string(error.message())));
}

}  // namespace

Result<bool> Win32PackageRemover::installed(const std::string& full_name) const {
  enter_apartment();
  try {
    const PackageManager manager;
    return manager.FindPackageForUser(winrt::hstring{}, winrt::to_hstring(full_name)) != nullptr;
  } catch (const winrt::hresult_error& error) {
    return failed("PackageManager.FindPackageForUser", error);
  }
}

Result<void> Win32PackageRemover::remove(const std::string& full_name,
                                         const std::stop_token& stop) {
  enter_apartment();
  try {
    const PackageManager manager;
    auto operation = manager.RemovePackageAsync(winrt::to_hstring(full_name));
    if (!operation) {
      return fail(Errc::io_error, "RemovePackageAsync started no operation");
    }
    using winrt::Windows::Foundation::AsyncStatus;
    while (operation.Status() == AsyncStatus::Started) {
      if (stop.stop_requested()) {
        operation.Cancel();
        return fail(Errc::io_error, "cancelled: the tray is quitting");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }
    const AsyncStatus status = operation.Status();
    const auto result = operation.GetResults();
    if (!result) {
      return fail(Errc::io_error, "RemovePackageAsync reported no result");
    }
    if (status != AsyncStatus::Completed || FAILED(result.ExtendedErrorCode())) {
      return fail(Errc::io_error,
                  std::format("RemovePackageAsync failed (0x{:08x}): {}",
                              static_cast<std::uint32_t>(result.ExtendedErrorCode().value),
                              winrt::to_string(result.ErrorText())));
    }
    return {};
  } catch (const winrt::hresult_error& error) {
    return failed("PackageManager.RemovePackageAsync", error);
  }
}

}  // namespace myclean::tray
