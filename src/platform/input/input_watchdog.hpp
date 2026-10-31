#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

#include <wil/resource.h>
#include <windows.h>

#include "core/input/chord_filter.hpp"
#include "core/input/probe.hpp"
#include "core/input/watchdog.hpp"
#include "core/model/result.hpp"
#include "core/os/clock.hpp"
#include "platform/input/win32_input_probe.hpp"

namespace myclean::platform {

// A message-only (HWND_MESSAGE) window: never visible, never in the taskbar, cannot take focus.
// It belongs to the thread that created it, which must also destroy it.
class MessageWindow {
 public:
  using Handler = std::function<void(UINT message, WPARAM wparam, LPARAM lparam)>;

  // `handler`, when set, sees every message before DefWindowProc.
  [[nodiscard]] static Result<MessageWindow> create(Handler handler = {});

  [[nodiscard]] HWND get() const { return window_.get(); }

 private:
  explicit MessageWindow(std::unique_ptr<Handler> handler) : handler_(std::move(handler)) {}

  std::unique_ptr<Handler> handler_;  // outlives window_, whose messages it handles
  wil::unique_hwnd window_;
};

// Registers the raw keyboard sink (RIDEV_INPUTSINK, keystrokes while another app has focus) and
// the clipboard format listener on `window`. Neither is a hook: input reaches applications
// without passing through MyClean. On failure nothing stays registered.
[[nodiscard]] Result<void> register_input_sinks(HWND window);
// Removes both registrations.
[[nodiscard]] Result<void> unregister_input_sinks(HWND window);

// The Ctrl+C / Ctrl+V freeze watchdog for the tray process (spec input-watchdog).
//
// The input thread (above-normal priority) owns a message-only window with both sinks, stamps
// every WM_INPUT and WM_CLIPBOARDUPDATE with the monotonic clock (QueryPerformanceCounter) on
// receipt, passes raw keys through ChordFilter, which drops everything but Ctrl+C / Ctrl+V,
// and queues the chords and clipboard updates in the Watchdog. The probing thread runs the
// Watchdog's rounds and hands each incident to the sink.
class InputWatchdog {
 public:
  // Starts both threads; fails when the window or a registration fails. `sink` and `clock`
  // must outlive the watchdog; `sink` is called from the probing thread.
  [[nodiscard]] static Result<std::unique_ptr<InputWatchdog>> start(input::IIncidentSink& sink,
                                                                    const os::IClock& clock);

  InputWatchdog(const InputWatchdog&) = delete;
  InputWatchdog& operator=(const InputWatchdog&) = delete;
  InputWatchdog(InputWatchdog&&) = delete;
  InputWatchdog& operator=(InputWatchdog&&) = delete;
  ~InputWatchdog();

  // Unregisters both sinks and joins both threads; a watch in progress is dropped. Idempotent;
  // call from one thread.
  void stop();

 private:
  InputWatchdog(input::IIncidentSink& sink, const os::IClock& clock);

  void run_input(std::promise<Result<void>>& started);
  void run_probes(const std::stop_token& stop);
  void on_message(UINT message, LPARAM lparam);
  void wake();

  const os::IClock* clock_;
  Win32InputProbe probe_;
  input::Watchdog watchdog_;
  input::ChordFilter filter_;  // input thread only

  std::mutex mutex_;  // guards woken_
  std::condition_variable_any wake_;
  bool woken_ = false;

  DWORD input_thread_id_ = 0;
  std::thread input_;
  std::jthread prober_;
};

}  // namespace myclean::platform
