#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/input/incident.hpp"

namespace myclean::input {

// A window, the thread that owns it and that thread's process.
struct WindowRef {
  std::uint64_t handle = 0;
  std::uint32_t thread = 0;
  ProcessRef process;
};

// Read-only access to the window manager and the clipboard state. No call opens the clipboard,
// reads clipboard data, or sends anything but WM_NULL. Calls may block (answers() for up to its
// timeout) and are made from the watchdog's probing thread only.
class IInputProbe {
 public:
  virtual ~IInputProbe() = default;
  IInputProbe(const IInputProbe&) = delete;
  IInputProbe& operator=(const IInputProbe&) = delete;
  IInputProbe(IInputProbe&&) = delete;
  IInputProbe& operator=(IInputProbe&&) = delete;

  [[nodiscard]] virtual std::optional<WindowRef> foreground() = 0;
  [[nodiscard]] virtual std::optional<WindowRef> clipboard_owner() = 0;
  // The window that has the clipboard open right now.
  [[nodiscard]] virtual std::optional<WindowRef> open_clipboard_window() = 0;
  // The head of the legacy clipboard viewer chain.
  [[nodiscard]] virtual std::optional<WindowRef> clipboard_viewer() = 0;
  // SendMessageTimeout(WM_NULL) with `timeout`: true when the window's thread answered.
  [[nodiscard]] virtual bool answers(const WindowRef& window,
                                     std::chrono::milliseconds timeout) = 0;
  // The formats on the clipboard, in the order Windows lists them; only registered formats need
  // a name (see describe_formats).
  [[nodiscard]] virtual std::vector<ClipboardFormat> formats() = 0;
  // The processes with a visible top-level window that IsHungAppWindow reports hung.
  [[nodiscard]] virtual std::vector<ProcessRef> hung_windows() = 0;
  // The image names of all running processes.
  [[nodiscard]] virtual std::vector<std::string> running_images() = 0;
  [[nodiscard]] virtual std::optional<CpuTimes> cpu_times() = 0;
  [[nodiscard]] virtual std::optional<int> memory_load_percent() = 0;

 protected:
  IInputProbe() = default;
};

// Receives each incident once, from the watchdog's probing thread.
class IIncidentSink {
 public:
  virtual ~IIncidentSink() = default;
  IIncidentSink(const IIncidentSink&) = delete;
  IIncidentSink& operator=(const IIncidentSink&) = delete;
  IIncidentSink(IIncidentSink&&) = delete;
  IIncidentSink& operator=(IIncidentSink&&) = delete;

  virtual void record(const Incident& incident) = 0;

 protected:
  IIncidentSink() = default;
};

}  // namespace myclean::input
