#include "platform/input/input_watchdog.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include <wil/resource.h>
#include <windows.h>

#include "core/input/incident.hpp"
#include "core/input/probe.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"

namespace myclean::platform {

namespace {

constexpr const wchar_t* window_class = L"MyClean.InputWatchdog";
constexpr USHORT usage_page_generic = 0x01;
constexpr USHORT usage_keyboard = 0x06;

Error win32_error(const char* call) {
  return {
      .code = Errc::io_error,
      .message = std::string(call) + " failed: Win32 error " + std::to_string(::GetLastError())};
}

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  const auto* handler =
      std::bit_cast<const MessageWindow::Handler*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
  if (handler != nullptr && *handler) {
    (*handler)(message, wparam, lparam);
  }
  return ::DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

Result<MessageWindow> MessageWindow::create(Handler handler) {
  static const ATOM registered = [] {
    const WNDCLASSEXW description{.cbSize = sizeof(WNDCLASSEXW),
                                  .lpfnWndProc = window_procedure,
                                  .hInstance = ::GetModuleHandleW(nullptr),
                                  .lpszClassName = window_class};
    return ::RegisterClassExW(&description);
  }();
  if (registered == 0) {
    return tl::unexpected(win32_error("RegisterClassExW"));
  }
  MessageWindow result{std::make_unique<Handler>(std::move(handler))};
  result.window_.reset(::CreateWindowExW(0, window_class, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                         ::GetModuleHandleW(nullptr), nullptr));
  if (!result.window_) {
    return tl::unexpected(win32_error("CreateWindowExW"));
  }
  ::SetWindowLongPtrW(result.window_.get(), GWLP_USERDATA,
                      std::bit_cast<LONG_PTR>(result.handler_.get()));
  return result;
}

Result<void> register_input_sinks(HWND window) {
  const RAWINPUTDEVICE keyboard{.usUsagePage = usage_page_generic,
                                .usUsage = usage_keyboard,
                                .dwFlags = RIDEV_INPUTSINK,
                                .hwndTarget = window};
  if (::RegisterRawInputDevices(&keyboard, 1, sizeof(RAWINPUTDEVICE)) == FALSE) {
    return tl::unexpected(win32_error("RegisterRawInputDevices"));
  }
  if (::AddClipboardFormatListener(window) == FALSE) {
    const Error error = win32_error("AddClipboardFormatListener");
    const RAWINPUTDEVICE remove{
        .usUsagePage = usage_page_generic, .usUsage = usage_keyboard, .dwFlags = RIDEV_REMOVE};
    ::RegisterRawInputDevices(&remove, 1, sizeof(RAWINPUTDEVICE));
    return tl::unexpected(error);
  }
  return {};
}

Result<void> unregister_input_sinks(HWND window) {
  const bool listener_removed = ::RemoveClipboardFormatListener(window) != FALSE;
  const Error listener_error = win32_error("RemoveClipboardFormatListener");
  const RAWINPUTDEVICE remove{
      .usUsagePage = usage_page_generic, .usUsage = usage_keyboard, .dwFlags = RIDEV_REMOVE};
  if (::RegisterRawInputDevices(&remove, 1, sizeof(RAWINPUTDEVICE)) == FALSE) {
    return tl::unexpected(win32_error("RegisterRawInputDevices(RIDEV_REMOVE)"));
  }
  if (!listener_removed) {
    return tl::unexpected(listener_error);
  }
  return {};
}

InputWatchdog::InputWatchdog(input::IIncidentSink& sink, const os::IClock& clock)
    : clock_(&clock), watchdog_(probe_, clock, sink) {}

Result<std::unique_ptr<InputWatchdog>> InputWatchdog::start(input::IIncidentSink& sink,
                                                            const os::IClock& clock) {
  std::unique_ptr<InputWatchdog> watchdog{new InputWatchdog(sink, clock)};
  std::promise<Result<void>> started;
  std::future<Result<void>> ready = started.get_future();
  watchdog->input_ = std::thread([self = watchdog.get(), started = std::move(started)]() mutable {
    self->run_input(started);
  });
  const Result<void> result = ready.get();
  if (!result) {
    watchdog->input_.join();
    return tl::unexpected(result.error());
  }
  watchdog->prober_ = std::jthread(
      [self = watchdog.get()](const std::stop_token& stop) { self->run_probes(stop); });
  return watchdog;
}

InputWatchdog::~InputWatchdog() {
  stop();
}

void InputWatchdog::stop() {
  if (input_.joinable()) {
    ::PostThreadMessageW(input_thread_id_, WM_QUIT, 0, 0);
    input_.join();
  }
  if (prober_.joinable()) {
    prober_.request_stop();
    prober_.join();
  }
}

void InputWatchdog::run_input(std::promise<Result<void>>& started) {
  ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  Result<MessageWindow> window = MessageWindow::create(
      [this](UINT message, WPARAM /*wparam*/, LPARAM lparam) { on_message(message, lparam); });
  if (!window) {
    started.set_value(tl::unexpected(window.error()));
    return;
  }
  const Result<void> registered = register_input_sinks(window->get());
  if (!registered) {
    started.set_value(registered);
    return;
  }
  // The window gave this thread its message queue, so stop() can post WM_QUIT from now on.
  input_thread_id_ = ::GetCurrentThreadId();
  started.set_value({});

  MSG message{};
  while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
    ::DispatchMessageW(&message);
  }
  static_cast<void>(unregister_input_sinks(window->get()));
}

void InputWatchdog::on_message(UINT message, LPARAM lparam) {
  if (message == WM_CLIPBOARDUPDATE) {
    watchdog_.clipboard_updated(clock_->monotonic());
    wake();
    return;
  }
  if (message != WM_INPUT) {
    return;
  }
  const MonoTime received = clock_->monotonic();
  const DWORD received_ticks = ::GetTickCount();
  const auto message_ticks = static_cast<DWORD>(::GetMessageTime());

  RAWINPUT raw{};
  UINT size = sizeof(RAWINPUT);
  if (::GetRawInputData(std::bit_cast<HRAWINPUT>(lparam), RID_INPUT, &raw, &size,
                        sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) ||
      raw.header.dwType != RIM_TYPEKEYBOARD) {
    return;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): RIM_TYPEKEYBOARD selects it
  const RAWKEYBOARD& key = raw.data.keyboard;
  const std::optional<input::Chord> chord =
      filter_.on_key(key.VKey, (key.Flags & RI_KEY_E0) != 0, (key.Flags & RI_KEY_BREAK) != 0);
  if (!chord) {
    return;
  }
  // Signed difference of two wrapping tick counts; a message time ahead of the tick is 0.
  const auto lag = static_cast<std::int32_t>(received_ticks - message_ticks);
  watchdog_.key(*chord, received, std::chrono::milliseconds{std::max(lag, 0)});
  wake();
}

void InputWatchdog::run_probes(const std::stop_token& stop) {
  ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  std::optional<MonoTime> due;
  while (!stop.stop_requested()) {
    {
      std::unique_lock lock(mutex_);
      const auto woken = [this] { return woken_; };
      if (due) {
        wake_.wait_for(lock, stop, *due - clock_->monotonic(), woken);
      } else {
        wake_.wait(lock, stop, woken);
      }
      woken_ = false;
    }
    if (stop.stop_requested()) {
      return;
    }
    due = watchdog_.poll();
  }
}

void InputWatchdog::wake() {
  {
    const std::scoped_lock lock(mutex_);
    woken_ = true;
  }
  wake_.notify_one();
}

}  // namespace myclean::platform
