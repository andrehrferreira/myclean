#include "tray/tray_icon.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string_view>

// clang-format off
#include <windows.h>  // first: shellapi.h needs its types
#include <shellapi.h>
// clang-format on

#include "core/model/result.hpp"

namespace myclean::tray {

namespace {

constexpr UINT icon_id = 1;

}  // namespace

NOTIFYICONDATAW TrayIcon::data(HICON icon, std::string_view tooltip) const {
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = window_;
  data.uID = icon_id;
  data.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
  data.uCallbackMessage = callback_message;
  data.hIcon = icon;
  // The tooltip is ASCII (tray_model::tooltip_text), so each byte is one character.
  const std::size_t length = std::min(tooltip.size(), std::size(data.szTip) - 1);
  std::ranges::copy(tooltip.substr(0, length), std::begin(data.szTip));
  return data;
}

Result<void> TrayIcon::add(HICON icon, std::string_view tooltip) {
  NOTIFYICONDATAW icon_data = data(icon, tooltip);
  remove();
  if (::Shell_NotifyIconW(NIM_ADD, &icon_data) == FALSE) {
    return fail(Errc::io_error, "Shell_NotifyIcon(NIM_ADD) failed");
  }
  added_ = true;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): NOTIFYICONDATA's version member
  icon_data.uVersion = NOTIFYICON_VERSION_4;
  if (::Shell_NotifyIconW(NIM_SETVERSION, &icon_data) == FALSE) {
    return fail(Errc::io_error, "Shell_NotifyIcon(NIM_SETVERSION) failed");
  }
  return {};
}

Result<void> TrayIcon::update(HICON icon, std::string_view tooltip) {
  NOTIFYICONDATAW icon_data = data(icon, tooltip);
  if (::Shell_NotifyIconW(NIM_MODIFY, &icon_data) == FALSE) {
    return fail(Errc::io_error, "Shell_NotifyIcon(NIM_MODIFY) failed");
  }
  return {};
}

void TrayIcon::remove() {
  if (!added_) {
    return;
  }
  NOTIFYICONDATAW icon_data{};
  icon_data.cbSize = sizeof(icon_data);
  icon_data.hWnd = window_;
  icon_data.uID = icon_id;
  ::Shell_NotifyIconW(NIM_DELETE, &icon_data);
  added_ = false;
}

}  // namespace myclean::tray
