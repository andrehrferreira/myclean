#pragma once

#include <string_view>

// clang-format off
#include <windows.h>  // first: shellapi.h needs its types
#include <shellapi.h>
// clang-format on

#include "core/model/result.hpp"

namespace myclean::tray {

// The notification-area icon of `window`, which receives callback_message with the
// NOTIFYICON_VERSION_4 layout: LOWORD(lParam) is the event (NIN_SELECT, WM_CONTEXTMENU, ...)
// and wParam the anchor point in screen coordinates.
class TrayIcon {
 public:
  static constexpr UINT callback_message = WM_APP + 1;

  explicit TrayIcon(HWND window) : window_(window) {}
  TrayIcon(const TrayIcon&) = delete;
  TrayIcon& operator=(const TrayIcon&) = delete;
  TrayIcon(TrayIcon&&) = delete;
  TrayIcon& operator=(TrayIcon&&) = delete;
  ~TrayIcon() { remove(); }

  // Adds the icon, or adds it again after Explorer restarted (the "TaskbarCreated" message).
  [[nodiscard]] Result<void> add(HICON icon, std::string_view tooltip);
  // Changes the image and the tooltip (ASCII, cut to what the shell keeps).
  [[nodiscard]] Result<void> update(HICON icon, std::string_view tooltip);
  void remove();

 private:
  [[nodiscard]] NOTIFYICONDATAW data(HICON icon, std::string_view tooltip) const;

  HWND window_;
  bool added_ = false;
};

}  // namespace myclean::tray
