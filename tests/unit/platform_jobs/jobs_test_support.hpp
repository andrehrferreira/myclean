#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/os/jobs.hpp"
#include "platform/jobs/win32_jobs.hpp"
#include "workload_process.hpp"

namespace myclean::platform::testing {

// Skips when the antivirus blocks the workload helper; owns a job control that expects the
// current user as job owner, and releases every job a test created, so no permanent job is left
// behind when the tests run elevated as LocalSystem.
class JobsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static const std::optional<std::string> blocked = antivirus_blocks_workload();
    if (blocked) {
      GTEST_SKIP() << *blocked;
    }
    const Result<std::string> sid = current_user_sid();
    ASSERT_TRUE(sid) << sid.error().message;
    control_ = std::make_unique<Win32JobControl>(*sid);
  }

  void TearDown() override {
    if (!control_) {
      return;
    }
    for (const os::JobRef& ref : created_) {
      if (Result<std::unique_ptr<os::IJob>> job = control_->open(ref); job) {
        (void)(*job)->release();
      }
    }
  }

  // A new job of class "test" (or `job_class`); null after a failed test assertion.
  std::unique_ptr<os::IJob> create(std::string_view job_class = "test") {
    Result<std::unique_ptr<os::IJob>> job = control_->create(job_class);
    EXPECT_TRUE(job) << job.error().message;
    if (!job) {
      return nullptr;
    }
    created_.push_back((*job)->ref());
    return std::move(*job);
  }

  [[nodiscard]] Win32JobControl& control() { return *control_; }

 private:
  std::unique_ptr<Win32JobControl> control_;
  std::vector<os::JobRef> created_;
};

}  // namespace myclean::platform::testing
