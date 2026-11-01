#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <wil/resource.h>
#include <windows.h>

#include "core/os/process_control.hpp"

namespace myclean::platform::testing {

// One run of the workload helper (workload.cpp), started without a window at below-normal
// priority. Terminated and waited for when destroyed, so a failing test leaves nothing running.
class Workload {
 public:
  // How a start failed.
  enum class Failure { none, blocked_by_antivirus, other };

  // Starts `workload <arguments>`; suspended (CREATE_SUSPENDED) when asked.
  static Workload start(std::wstring_view arguments, bool suspended = false);

  ~Workload();
  Workload(Workload&& other) noexcept = default;
  Workload& operator=(Workload&& other) = delete;
  Workload(const Workload&) = delete;
  Workload& operator=(const Workload&) = delete;

  [[nodiscard]] bool started() const { return static_cast<bool>(process_); }
  [[nodiscard]] Failure failure() const { return failure_; }
  [[nodiscard]] const std::string& error() const { return error_; }

  [[nodiscard]] const os::ProcessKey& key() const { return key_; }
  [[nodiscard]] HANDLE process() const { return process_.get(); }
  // Releases the CREATE_SUSPENDED start.
  void resume() const;
  // The exit code, or nothing when the process still runs after `timeout`.
  [[nodiscard]] std::optional<std::uint32_t> wait(std::chrono::milliseconds timeout) const;
  // User + kernel time of the process so far.
  [[nodiscard]] std::chrono::nanoseconds cpu_time() const;

 private:
  Workload() = default;

  wil::unique_handle process_;
  wil::unique_handle thread_;
  os::ProcessKey key_;
  Failure failure_ = Failure::none;
  std::string error_;
};

// Launches the helper once to see that the antivirus lets it run (spike X4 §3.8). Empty when it
// does; otherwise why not.
[[nodiscard]] std::optional<std::string> antivirus_blocks_workload();

}  // namespace myclean::platform::testing
