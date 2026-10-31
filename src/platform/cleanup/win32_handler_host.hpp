#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <wil/com.h>
#include <wil/resource.h>
#include <windows.h>
// clang-format off
#include <emptyvc.h>  // after windows.h
// clang-format on

#include "core/cleanup/handler_host.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::platform {

// A handler object and the key it is registered under.
struct LoadedHandler {
  wil::com_ptr_nothrow<IEmptyVolumeCache> cache;
  wil::unique_hkey key;  // opened read only; passed to Initialize
  std::string clsid;
};

// Creates the handler registered under `name`. Called on the host's worker thread, in its
// apartment.
using HandlerLoader = std::function<Result<LoadedHandler>(const std::wstring& name)>;

// What one call into a handler did (X3 spike, docs/spikes/X3-cleanup-handlers.md).
struct HandlerProbe {
  std::string name;
  std::string clsid;
  std::string display_name;
  bool loaded = false;
  bool initialize_ex = false;  // through IEmptyVolumeCache2::InitializeEx
  HRESULT initialize_result = S_OK;
  std::uint32_t flags = 0;      // EVCF_* returned by Initialize
  HRESULT scan_result = S_OK;   // GetSpaceUsed; not called when Initialize returned S_FALSE
  HRESULT purge_result = S_OK;  // Purge; only for purge()
  Bytes space_used;
  bool showed_window = false;  // a visible window appeared on the private desktop
  bool timed_out = false;
  bool cancelled = false;
  bool abandoned = false;  // the call did not end after the abort; its thread was left
  std::chrono::milliseconds elapsed{};
  std::optional<Error> error;  // why the handler could not be loaded or called
};

// Runs the disk-cleanup handlers registered under HKLM\SOFTWARE\Microsoft\Windows\
// CurrentVersion\Explorer\VolumeCaches (IEmptyVolumeCache). Each call runs on a fresh
// single-threaded-apartment thread assigned to a private desktop of this process, so any window
// a handler opens is never shown to the user; a visible window there aborts the call, which
// then fails with refused. Initialize never gets EVCF_SETTINGSMODE (that mode deletes inside
// Initialize) nor any other input flag. A call that exceeds its timeout or whose `stop` is
// requested is aborted through the progress callback; when the handler ignores that too, its
// thread is abandoned and keeps running in the background.
class Win32HandlerHost final : public cleanup::IHandlerHost {
 public:
  // `loader` defaults to the registry: the key's default value is the handler's CLSID,
  // created in process.
  Win32HandlerHost(std::chrono::milliseconds scan_timeout, std::chrono::milliseconds purge_timeout,
                   HandlerLoader loader = {});
  ~Win32HandlerHost() override;
  Win32HandlerHost(const Win32HandlerHost&) = delete;
  Win32HandlerHost& operator=(const Win32HandlerHost&) = delete;
  Win32HandlerHost(Win32HandlerHost&&) = delete;
  Win32HandlerHost& operator=(Win32HandlerHost&&) = delete;

  // The names of the registered handlers, sorted.
  [[nodiscard]] static Result<std::vector<std::string>> registered_handlers();

  // Initialize and GetSpaceUsed, with everything that happened. Nothing is removed.
  [[nodiscard]] HandlerProbe probe(std::string_view handler, const std::filesystem::path& volume,
                                   const std::stop_token& stop);

  [[nodiscard]] Result<Bytes> space_used(std::string_view handler,
                                         const std::filesystem::path& volume,
                                         std::stop_token stop) override;
  [[nodiscard]] Result<void> purge(std::string_view handler, const std::filesystem::path& volume,
                                   std::stop_token stop) override;

 private:
  [[nodiscard]] HandlerProbe call(std::string_view handler, const std::filesystem::path& volume,
                                  bool purge, const std::stop_token& stop);

  std::chrono::milliseconds scan_timeout_;
  std::chrono::milliseconds purge_timeout_;
  HandlerLoader loader_;
  HDESK desktop_ = nullptr;
};

// The result of a probe or purge as the IHandlerHost contract states it.
[[nodiscard]] Result<void> probe_status(const HandlerProbe& probe);

}  // namespace myclean::platform
