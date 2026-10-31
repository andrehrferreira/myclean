#include "tray/instance.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"

namespace myclean::tray {

namespace {

tl::unexpected<Error> win32_failure(std::string_view action, DWORD error) {
  return fail(error == ERROR_ACCESS_DENIED ? Errc::access_denied : Errc::io_error,
              std::string(action) + ": " + std::system_category().message(static_cast<int>(error)));
}

}  // namespace

Result<std::optional<Instance>> Instance::acquire(std::wstring_view mutex_name,
                                                  std::wstring_view event_name) {
  const std::wstring mutex_text(mutex_name);
  wil::unique_mutex_nothrow mutex(::CreateMutexW(nullptr, TRUE, mutex_text.c_str()));
  const DWORD mutex_error = ::GetLastError();
  if (!mutex) {
    // Another account's tray in this session holds a mutex this one may not open.
    if (mutex_error == ERROR_ACCESS_DENIED) {
      return std::optional<Instance>();
    }
    return win32_failure("CreateMutex", mutex_error);
  }
  if (mutex_error == ERROR_ALREADY_EXISTS) {
    // Not the owner: initial ownership is not granted for an existing mutex.
    return std::optional<Instance>();
  }
  const std::wstring event_text(event_name);
  wil::unique_event_nothrow quit(::CreateEventW(nullptr, TRUE, FALSE, event_text.c_str()));
  if (!quit) {
    return win32_failure("CreateEvent", ::GetLastError());
  }
  quit.ResetEvent();
  return std::optional<Instance>(Instance(std::move(mutex), std::move(quit)));
}

bool Instance::quit_requested() const {
  return ::WaitForSingleObject(quit_.get(), 0) == WAIT_OBJECT_0;
}

}  // namespace myclean::tray
