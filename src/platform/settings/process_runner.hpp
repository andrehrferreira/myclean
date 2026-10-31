#pragma once

#include <chrono>

#include "core/model/result.hpp"
#include "core/placement/settings.hpp"

namespace myclean::platform {

// Runs a tool of the Windows system folder (GetSystemDirectoryW) with CREATE_NO_WINDOW, its
// standard output and error captured and converted from the OEM code page. A tool still running
// after `timeout` is terminated and reported as failed.
class Win32ProcessRunner final : public placement::IProcessRunner {
 public:
  explicit Win32ProcessRunner(std::chrono::milliseconds timeout = std::chrono::minutes{2})
      : timeout_(timeout) {}

  [[nodiscard]] Result<placement::CommandResult> run(const placement::Command& command) override;

 private:
  std::chrono::milliseconds timeout_;
};

}  // namespace myclean::platform
