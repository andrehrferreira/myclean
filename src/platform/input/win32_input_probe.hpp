#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "core/input/incident.hpp"
#include "core/input/probe.hpp"

namespace myclean::platform {

// The watchdog's probes over user32: GetForegroundWindow, GetClipboardOwner,
// GetOpenClipboardWindow, GetClipboardViewer, GetWindowThreadProcessId,
// SendMessageTimeout(WM_NULL), GetUpdatedClipboardFormats, IsHungAppWindow, a Toolhelp process
// snapshot, GetSystemTimes and GlobalMemoryStatusEx. It never calls OpenClipboard.
class Win32InputProbe final : public input::IInputProbe {
 public:
  Win32InputProbe() = default;

  [[nodiscard]] std::optional<input::WindowRef> foreground() override;
  [[nodiscard]] std::optional<input::WindowRef> clipboard_owner() override;
  [[nodiscard]] std::optional<input::WindowRef> open_clipboard_window() override;
  [[nodiscard]] std::optional<input::WindowRef> clipboard_viewer() override;
  [[nodiscard]] bool answers(const input::WindowRef& window,
                             std::chrono::milliseconds timeout) override;
  [[nodiscard]] std::vector<input::ClipboardFormat> formats() override;
  [[nodiscard]] std::vector<input::ProcessRef> hung_windows() override;
  [[nodiscard]] std::vector<std::string> running_images() override;
  [[nodiscard]] std::optional<input::CpuTimes> cpu_times() override;
  [[nodiscard]] std::optional<int> memory_load_percent() override;
};

}  // namespace myclean::platform
