#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "core/model/result.hpp"
#include "core/os/jobs.hpp"
#include "core/os/process_control.hpp"

// Win32 side of the governor's job objects (core/os/jobs.hpp). Measured behaviour of every API
// used here: docs/spikes/X4-jobs.md.
namespace myclean::platform {

// LocalSystem, the owner every job opened by the service must have.
inline constexpr std::string_view local_system_sid = "S-1-5-18";

// Jobs are created with NtCreateJobObject under \BaseNamedObjects (the Global\ namespace), owned
// by this process's user (set explicitly, so the owner does not depend on the token's default
// owner), with OBJ_PERMANENT when SeCreatePermanentPrivilege can be enabled. The ntdll functions
// are resolved at run time.
class Win32JobControl final : public os::IJobControl {
 public:
  // `expected_owner` is the SID string that the owner of every opened job must equal: SYSTEM in
  // the service, the current user in tests (current_user_sid()). Enables
  // SeCreatePermanentPrivilege in this process's token when the token holds it.
  explicit Win32JobControl(std::string expected_owner = std::string(local_system_sid));

  [[nodiscard]] Result<std::unique_ptr<os::IJob>> create(std::string_view job_class) override;
  [[nodiscard]] Result<std::unique_ptr<os::IJob>> open(const os::JobRef& ref) override;
  [[nodiscard]] Result<bool> in_any_job(const os::ProcessKey& process) const override;

  // True when created jobs are permanent: this process holds SeCreatePermanentPrivilege.
  [[nodiscard]] bool creates_permanent() const { return permanent_; }

 private:
  std::string expected_owner_;
  bool permanent_ = false;
};

// The SID of the user this process runs as ("S-1-5-21-..." or "S-1-5-18").
[[nodiscard]] Result<std::string> current_user_sid();

}  // namespace myclean::platform
