#include "platform/apps/win32_apps.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/apps/apps.hpp"
#include "core/model/result.hpp"
#include "platform/ipc/security.hpp"

namespace myclean::platform {
namespace {

// Read-only against this machine as the test's own user: nothing is started, removed or changed.
TEST(Win32AppSystem, ReadsTheCallersPackagesUsageAndTheMaker) {
  const Result<std::string> sid = current_user_sid();
  ASSERT_TRUE(sid.has_value()) << sid.error().message;
  const Win32AppSystem system(*sid);

  const Result<std::vector<apps::AppxPackage>> packages = system.appx();
  ASSERT_TRUE(packages.has_value()) << packages.error().message;
  for (const apps::AppxPackage& package : *packages) {
    EXPECT_FALSE(package.full_name.empty());
    EXPECT_FALSE(package.family_name.empty());
  }
  const Result<std::vector<apps::RunRecord>> assisted = system.user_assist();
  ASSERT_TRUE(assisted.has_value()) << assisted.error().message;
  for (const apps::RunRecord& record : *assisted) {
    EXPECT_EQ(record.source, apps::UsageSource::user_assist);
    EXPECT_FALSE(record.path.empty());
  }
  const Result<std::string> maker = system.manufacturer();
  ASSERT_TRUE(maker.has_value()) << maker.error().message;
  EXPECT_FALSE(maker->empty());
  // Prefetch and BAM need administrator rights; without them they say so.
  const Result<std::vector<apps::RunRecord>> prefetched = system.prefetch();
  EXPECT_TRUE(prefetched.has_value() || prefetched.error().code == Errc::access_denied)
      << prefetched.error().message;
  const Result<std::vector<apps::RunRecord>> moderated = system.bam();
  EXPECT_TRUE(moderated.has_value() || moderated.error().code == Errc::access_denied)
      << moderated.error().message;
}

}  // namespace
}  // namespace myclean::platform
