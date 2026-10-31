#include "platform/cleanup/win32_handler_host.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <wil/com.h>
#include <wil/resource.h>
#include <windows.h>
// clang-format off
#include <emptyvc.h>  // after windows.h
#include <wrl/implements.h>
// clang-format on

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "platform/cleanup/wide.hpp"

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;

constexpr std::wstring_view volume_caches =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VolumeCaches";
constexpr auto poll = std::chrono::milliseconds(50);
constexpr auto max_grace = std::chrono::seconds(30);  // wait after an abort before abandoning
constexpr std::size_t clsid_length = 64;

// Shared by a call and its worker thread, which outlives the call when it is abandoned.
struct CallState {
  std::mutex mutex;
  std::condition_variable finished_changed;
  bool finished = false;  // guarded by mutex; `probe` is the worker's until then
  std::atomic<bool> abort{false};
  std::atomic<bool> window_seen{false};
  HandlerProbe probe;
};

// Answers the handler's progress reports; E_ABORT asks it to stop.
class Callback final : public Microsoft::WRL::RuntimeClass<
                           Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                           IEmptyVolumeCacheCallBack> {
 public:
  explicit Callback(std::shared_ptr<CallState> state) : state_(std::move(state)) {}

  HRESULT STDMETHODCALLTYPE ScanProgress(DWORDLONG /*used*/, DWORD /*flags*/,
                                         LPCWSTR /*status*/) override {
    return state_->abort ? E_ABORT : S_OK;
  }
  HRESULT STDMETHODCALLTYPE PurgeProgress(DWORDLONG /*freed*/, DWORDLONG /*to_free*/,
                                          DWORD /*flags*/, LPCWSTR /*status*/) override {
    return state_->abort ? E_ABORT : S_OK;
  }

 private:
  std::shared_ptr<CallState> state_;
};

BOOL CALLBACK note_visible(HWND window, LPARAM found) {
  if (::IsWindowVisible(window) == FALSE) {
    return TRUE;
  }
  // The context is the bool of has_visible_window.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
  *reinterpret_cast<bool*>(found) = true;
  return FALSE;
}

bool has_visible_window(HDESK desktop) {
  bool found = false;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): EnumDesktopWindows context
  ::EnumDesktopWindows(desktop, note_visible, reinterpret_cast<LPARAM>(&found));
  return found;
}

Error hresult_error(std::string_view what, HRESULT result) {
  const bool denied = result == E_ACCESSDENIED ||
                      result == HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED) ||
                      result == HRESULT_FROM_WIN32(ERROR_PRIVILEGE_NOT_HELD);
  return {
      .code = denied ? Errc::access_denied : Errc::io_error,
      .message = std::format("{} failed with 0x{:08X}", what, static_cast<std::uint32_t>(result))};
}

Result<LoadedHandler> load_from_registry(const std::wstring& name) {
  if (name.empty() || name.find(L'\\') != std::wstring::npos) {
    return fail(Errc::invalid_argument, "not a handler name: " + narrow(name));
  }
  LoadedHandler loaded;
  const std::wstring path = std::wstring(volume_caches) + L"\\" + name;
  const LSTATUS opened = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0,
                                         KEY_READ | KEY_WOW64_64KEY, loaded.key.put());
  if (opened == ERROR_FILE_NOT_FOUND) {
    return fail(Errc::not_found, "no cleanup handler is registered as " + narrow(name));
  }
  if (opened != ERROR_SUCCESS) {
    return tl::unexpected(hresult_error("RegOpenKeyEx", HRESULT_FROM_WIN32(opened)));
  }
  std::array<wchar_t, clsid_length> text{};
  auto size = static_cast<DWORD>(text.size() * sizeof(wchar_t));
  const LSTATUS read = ::RegGetValueW(loaded.key.get(), nullptr, nullptr, RRF_RT_REG_SZ, nullptr,
                                      text.data(), &size);
  CLSID clsid{};
  if (read != ERROR_SUCCESS || FAILED(::CLSIDFromString(text.data(), &clsid))) {
    return fail(Errc::invalid_data, narrow(name) + " has no CLSID");
  }
  loaded.clsid = narrow(text.data());
  if (const HRESULT created = ::CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
                                                 IID_PPV_ARGS(loaded.cache.put()));
      FAILED(created)) {
    return tl::unexpected(hresult_error("CoCreateInstance " + loaded.clsid, created));
  }
  return loaded;
}

// The handler's calls, on the worker thread. Releases every COM object before returning.
void call_handler(CallState& state, const std::shared_ptr<CallState>& shared,
                  const HandlerLoader& loader, const std::wstring& name, const std::wstring& volume,
                  bool purge, HDESK desktop) {
  HandlerProbe& probe = state.probe;
  Result<LoadedHandler> loaded = loader(name);
  if (!loaded) {
    probe.error = loaded.error();
    return;
  }
  probe.loaded = true;
  probe.clsid = loaded->clsid;
  wil::unique_cotaskmem_string display;
  wil::unique_cotaskmem_string description;
  wil::unique_cotaskmem_string button;
  DWORD flags = 0;  // no input flag: EVCF_SETTINGSMODE would make the handler delete right here
  if (const auto extended = loaded->cache.try_query<IEmptyVolumeCache2>()) {
    probe.initialize_ex = true;
    probe.initialize_result =
        extended->InitializeEx(loaded->key.get(), volume.c_str(), name.c_str(), display.put(),
                               description.put(), button.put(), &flags);
  } else {
    probe.initialize_result = loaded->cache->Initialize(loaded->key.get(), volume.c_str(),
                                                        display.put(), description.put(), &flags);
  }
  probe.flags = flags;
  if (display) {
    probe.display_name = narrow(display.get());
  }
  const auto stopped = [&] {
    if (has_visible_window(desktop)) {
      state.window_seen = true;
    }
    return state.abort || state.window_seen;
  };
  if (probe.initialize_result != S_OK || stopped()) {
    return;
  }
  const auto callback = Microsoft::WRL::Make<Callback>(shared);
  DWORDLONG used = 0;
  probe.scan_result = loaded->cache->GetSpaceUsed(&used, callback.Get());
  probe.space_used = Bytes{used};
  if (!purge || probe.scan_result != S_OK || used == 0 || stopped()) {
    return;
  }
  probe.purge_result = loaded->cache->Purge(used, callback.Get());
  DWORD deactivated = 0;
  loaded->cache->Deactivate(&deactivated);
  stopped();
}

void worker(const std::shared_ptr<CallState>& state, const HandlerLoader& loader,
            const std::wstring& name, const std::wstring& volume, bool purge, HDESK desktop) {
  if (::SetThreadDesktop(desktop) == FALSE) {
    state->probe.error = hresult_error("SetThreadDesktop", HRESULT_FROM_WIN32(::GetLastError()));
  } else if (const HRESULT com =
                 ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
             FAILED(com)) {
    state->probe.error = hresult_error("CoInitializeEx", com);
  } else {
    call_handler(*state, state, loader, name, volume, purge, desktop);
    ::CoUninitialize();
  }
  {
    const std::scoped_lock lock(state->mutex);
    state->finished = true;
  }
  state->finished_changed.notify_all();
}

}  // namespace

Win32HandlerHost::Win32HandlerHost(std::chrono::milliseconds scan_timeout,
                                   std::chrono::milliseconds purge_timeout, HandlerLoader loader)
    : scan_timeout_(scan_timeout),
      purge_timeout_(purge_timeout),
      loader_(loader ? std::move(loader) : HandlerLoader{load_from_registry}) {
  static std::atomic<int> count{0};
  const std::wstring name = std::format(L"myclean-cleanup-{}-{}", ::GetCurrentProcessId(), count++);
  desktop_ = ::CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
}

Win32HandlerHost::~Win32HandlerHost() {
  if (desktop_ != nullptr) {
    ::CloseDesktop(desktop_);  // fails, leaving it open, while an abandoned thread still uses it
  }
}

Result<std::vector<std::string>> Win32HandlerHost::registered_handlers() {
  wil::unique_hkey key;
  if (const LSTATUS opened =
          ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, std::wstring(volume_caches).c_str(), 0,
                          KEY_READ | KEY_WOW64_64KEY, key.put());
      opened != ERROR_SUCCESS) {
    return tl::unexpected(hresult_error("RegOpenKeyEx VolumeCaches", HRESULT_FROM_WIN32(opened)));
  }
  std::vector<std::string> names;
  std::array<wchar_t, MAX_PATH> name{};
  for (DWORD index = 0;; ++index) {
    auto length = static_cast<DWORD>(name.size());
    const LSTATUS read =
        ::RegEnumKeyExW(key.get(), index, name.data(), &length, nullptr, nullptr, nullptr, nullptr);
    if (read == ERROR_NO_MORE_ITEMS) {
      break;
    }
    if (read != ERROR_SUCCESS) {
      return tl::unexpected(hresult_error("RegEnumKeyEx VolumeCaches", HRESULT_FROM_WIN32(read)));
    }
    names.push_back(narrow(std::wstring(name.data(), length)));
  }
  std::ranges::sort(names);
  return names;
}

HandlerProbe Win32HandlerHost::call(std::string_view handler, const fs::path& volume, bool purge,
                                    const std::stop_token& stop) {
  const auto started = std::chrono::steady_clock::now();
  HandlerProbe probe;
  probe.name = std::string(handler);
  if (desktop_ == nullptr) {
    probe.error = Error{.code = Errc::io_error,
                        .message = "no private desktop: handlers are not run without one"};
    return probe;
  }
  auto state = std::make_shared<CallState>();
  std::thread thread(worker, state, loader_, widen(std::string(handler)), volume.wstring(), purge,
                     desktop_);

  const std::chrono::milliseconds timeout = purge ? purge_timeout_ : scan_timeout_;
  bool timed_out = false;
  bool cancelled = false;
  std::unique_lock lock(state->mutex);
  while (!state->finished_changed.wait_for(lock, poll, [&] { return state->finished; })) {
    if (has_visible_window(desktop_)) {
      state->window_seen = true;
    }
    cancelled = stop.stop_requested();
    timed_out = std::chrono::steady_clock::now() - started >= timeout;
    if (state->window_seen || cancelled || timed_out) {
      state->abort = true;
      state->finished_changed.wait_for(lock,
                                       std::min<std::chrono::milliseconds>(timeout, max_grace),
                                       [&] { return state->finished; });
      break;
    }
  }
  const bool finished = state->finished;
  lock.unlock();
  if (finished) {
    thread.join();
    probe = state->probe;
  } else {
    thread.detach();
    probe.abandoned = true;
  }
  probe.showed_window = state->window_seen;
  probe.timed_out = timed_out;
  probe.cancelled = cancelled;
  probe.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  return probe;
}

HandlerProbe Win32HandlerHost::probe(std::string_view handler, const fs::path& volume,
                                     const std::stop_token& stop) {
  return call(handler, volume, false, stop);
}

Result<Bytes> Win32HandlerHost::space_used(std::string_view handler, const fs::path& volume,
                                           std::stop_token stop) {
  const HandlerProbe probe = call(handler, volume, false, stop);
  if (Result<void> status = probe_status(probe); !status) {
    return tl::unexpected(status.error());
  }
  return probe.initialize_result == S_FALSE ? Bytes{} : probe.space_used;
}

Result<void> Win32HandlerHost::purge(std::string_view handler, const fs::path& volume,
                                     std::stop_token stop) {
  return probe_status(call(handler, volume, true, stop));
}

Result<void> probe_status(const HandlerProbe& probe) {
  if (probe.showed_window) {
    return fail(Errc::refused, probe.name + " shows a window, which a headless run cannot answer");
  }
  if (probe.cancelled) {
    return fail(Errc::refused, probe.name + ": cancelled");
  }
  if (probe.timed_out) {
    return fail(Errc::io_error, probe.name + " timed out" +
                                    (probe.abandoned ? "; its thread was left running" : ""));
  }
  if (probe.error) {
    return fail(probe.error->code, probe.name + ": " + probe.error->message);
  }
  for (const auto& [what, result] :
       {std::pair{"Initialize", probe.initialize_result},
        std::pair{"GetSpaceUsed", probe.scan_result}, std::pair{"Purge", probe.purge_result}}) {
    if (FAILED(result)) {
      const Error error = hresult_error(probe.name + ": " + what, result);
      return fail(error.code, error.message);
    }
  }
  return {};
}

}  // namespace myclean::platform
