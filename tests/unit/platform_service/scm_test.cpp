#include "platform/service/scm.hpp"

#include <functional>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/model/result.hpp"

namespace myclean::platform {
namespace {

// The SCM part itself needs an installed service; here only its refusal outside the SCM is
// checked, which is what tells myclean-svc.exe it was started by hand.
TEST(ScmTest, RefusesToRunWhenNotStartedByTheScm) {
  bool ran = false;
  const Result<int> code =
      run_service(L"MyCleanTest", [&ran](HANDLE, const std::function<void()>&) {
        ran = true;
        return 0;
      });
  ASSERT_FALSE(code.has_value());
  EXPECT_EQ(code.error().code, Errc::invalid_state);
  EXPECT_FALSE(ran);
}

}  // namespace
}  // namespace myclean::platform
