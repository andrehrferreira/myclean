#include "core/os/jobs.hpp"

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/os/process_control.hpp"

namespace myclean::os {
namespace {

constexpr const char* nonce = "0123456789abcdef0123456789abcdef";

TEST(JobRef, ObjectNameIsInTheGlobalNamespace) {
  const JobRef ref{.job_class = "build", .nonce = nonce};
  EXPECT_EQ(ref.object_name(), "Global\\MyClean.Job.build.0123456789abcdef0123456789abcdef");
  EXPECT_TRUE(validate(ref));
}

TEST(JobRef, MalformedPartsAreRejected) {
  for (const char* job_class :
       {"", "Build", "a b", "a\\b", "a.b", "a-very-long-class-name-of-33-chars"}) {
    const Result<void> valid = validate(JobRef{.job_class = job_class, .nonce = nonce});
    ASSERT_FALSE(valid) << job_class;
    EXPECT_EQ(valid.error().code, Errc::invalid_argument);
  }
  for (const char* bad_nonce :
       {"", "0123", "0123456789ABCDEF0123456789ABCDEF", "0123456789abcdef0123456789abcdeg"}) {
    EXPECT_FALSE(validate(JobRef{.job_class = "build", .nonce = bad_nonce})) << bad_nonce;
  }
  EXPECT_TRUE(validate_job_class("cap_job-2"));
}

TEST(ProcessExit, CommitFailureCodes) {
  EXPECT_TRUE(is_commit_failure_exit(0xC0000017U));
  EXPECT_TRUE(is_commit_failure_exit(0xC0000409U));
  EXPECT_FALSE(is_commit_failure_exit(0U));
  EXPECT_FALSE(is_commit_failure_exit(0xC0000005U));
}

}  // namespace
}  // namespace myclean::os
