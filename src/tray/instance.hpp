#pragma once

#include <optional>
#include <string_view>
#include <utility>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"

namespace myclean::tray {

// The names of deploy contract C5. Tests use their own names, never these.
inline constexpr std::wstring_view instance_mutex_name = L"Local\\MyClean.Tray.Instance";
inline constexpr std::wstring_view quit_event_name = L"Local\\MyClean.Tray.Quit";

// The tray's claim to its session (single instance) and the event that asks it to quit.
class Instance {
 public:
  // nullopt when another process already holds `mutex_name`: that instance keeps running and
  // this one must exit. Otherwise holds the mutex and opens (or creates) the manual-reset quit
  // event `event_name`. A signal left on that event from a previous instance is cleared: holding
  // the mutex proves the quit was meant for the instance that is gone.
  [[nodiscard]] static Result<std::optional<Instance>> acquire(std::wstring_view mutex_name,
                                                               std::wstring_view event_name);

  // Signaled when the tray must quit; wait on it together with the message queue.
  [[nodiscard]] HANDLE quit_event() const { return quit_.get(); }
  [[nodiscard]] bool quit_requested() const;

 private:
  Instance(wil::unique_mutex_nothrow mutex, wil::unique_event_nothrow quit)
      : mutex_(std::move(mutex)), quit_(std::move(quit)) {}

  // Declared first so it is released last, after the quit event is closed.
  wil::unique_mutex_nothrow mutex_;
  wil::unique_event_nothrow quit_;
};

}  // namespace myclean::tray
