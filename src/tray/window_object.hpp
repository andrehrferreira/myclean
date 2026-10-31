#pragma once

#include <windows.h>

namespace myclean::tray {

// The object a window procedure serves: the last argument of CreateWindowEx, kept in the
// window's user data from WM_NCCREATE on. Null for the few messages before WM_NCCREATE.
template <typename T>
T* window_object(HWND window, UINT message, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    // NOLINTNEXTLINE(*-reinterpret-cast,performance-no-int-to-ptr): Win32 API
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): user data holds a pointer
    ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  // NOLINTNEXTLINE(*-reinterpret-cast,performance-no-int-to-ptr): see above
  return reinterpret_cast<T*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
}

}  // namespace myclean::tray
