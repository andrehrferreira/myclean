#include <optional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "platform/input/input_watchdog.hpp"

namespace myclean::platform {
namespace {

// The keyboard registration of this process, if any.
std::optional<RAWINPUTDEVICE> registered_keyboard() {
  UINT count = 0;
  if (::GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) ==
      static_cast<UINT>(-1)) {
    ADD_FAILURE() << "GetRegisteredRawInputDevices failed: " << ::GetLastError();
    return std::nullopt;
  }
  std::vector<RAWINPUTDEVICE> devices(count);
  if (count > 0 && ::GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE)) ==
                       static_cast<UINT>(-1)) {
    ADD_FAILURE() << "GetRegisteredRawInputDevices failed: " << ::GetLastError();
    return std::nullopt;
  }
  for (const RAWINPUTDEVICE& device : devices) {
    if (device.usUsagePage == 0x01 && device.usUsage == 0x06) {
      return device;
    }
  }
  return std::nullopt;
}

// Runs on its own thread, which never retrieves a message: any WM_INPUT or WM_CLIPBOARDUPDATE
// queued while registered is discarded unread with the thread's queue.
void register_and_unregister() {
  Result<MessageWindow> window = MessageWindow::create();
  ASSERT_TRUE(window.has_value()) << window.error().message;
  HWND__* const handle = window->get();
  EXPECT_FALSE(::IsWindowVisible(handle));
  EXPECT_NE(::GetAncestor(handle, GA_PARENT), ::GetDesktopWindow()) << "not message-only";

  const Result<void> registered = register_input_sinks(handle);
  ASSERT_TRUE(registered.has_value()) << registered.error().message;
  const std::optional<RAWINPUTDEVICE> keyboard = registered_keyboard();
  ASSERT_TRUE(keyboard.has_value());
  const RAWINPUTDEVICE device = keyboard.value_or(RAWINPUTDEVICE{});
  EXPECT_EQ(device.hwndTarget, handle);
  EXPECT_EQ(device.dwFlags & RIDEV_INPUTSINK, static_cast<DWORD>(RIDEV_INPUTSINK));

  const Result<void> unregistered = unregister_input_sinks(handle);
  EXPECT_TRUE(unregistered.has_value()) << unregistered.error().message;
  EXPECT_FALSE(registered_keyboard().has_value());
}

TEST(InputSinks, RegisterAndUnregisterOnAMessageOnlyWindow) {
  std::thread worker(register_and_unregister);
  worker.join();
}

}  // namespace
}  // namespace myclean::platform
