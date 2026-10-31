#include "platform/ipc/security.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/model/result.hpp"

namespace myclean::platform {
namespace {

constexpr std::string_view owner = "S-1-5-21-1000000000-2000000000-3000000000-1001";

static_assert(pipe_client_access == 0x12019B);
static_assert((pipe_client_access & FILE_CREATE_PIPE_INSTANCE) == 0);
static_assert((pipe_client_access & (FILE_READ_DATA | FILE_WRITE_DATA)) ==
              (FILE_READ_DATA | FILE_WRITE_DATA));

std::vector<AllowedAce> aces_of(const std::string& sddl) {
  Result<wil::unique_hlocal_security_descriptor> descriptor = make_security_descriptor(sddl);
  EXPECT_TRUE(descriptor.has_value());
  if (!descriptor) {
    return {};
  }
  Result<std::vector<AllowedAce>> aces = allowed_aces(descriptor->get());
  EXPECT_TRUE(aces.has_value());
  return aces ? *aces : std::vector<AllowedAce>{};
}

TEST(PipeSecurityTest, ServicePipeGrantsSystemAllAndTheOwnerReadWriteOnly) {
  EXPECT_EQ(pipe_sddl(owner, false), "D:P(A;;FA;;;SY)(A;;0x12019b;;;" + std::string(owner) + ")");
  const std::vector<AllowedAce> aces = aces_of(pipe_sddl(owner, false));
  ASSERT_EQ(aces.size(), 2U);
  EXPECT_EQ(aces[0].sid, system_sid);
  EXPECT_EQ(aces[0].mask, static_cast<DWORD>(FILE_ALL_ACCESS));
  EXPECT_EQ(aces[1].sid, owner);
  EXPECT_EQ(aces[1].mask, pipe_client_access);
}

TEST(PipeSecurityTest, ConsolePipeGrantsTheServingOwnerAll) {
  const std::vector<AllowedAce> aces = aces_of(pipe_sddl(owner, true));
  ASSERT_EQ(aces.size(), 2U);
  EXPECT_EQ(aces[1].sid, owner);
  EXPECT_EQ(aces[1].mask, static_cast<DWORD>(FILE_ALL_ACCESS));
}

TEST(PipeSecurityTest, ReadsTheCurrentUser) {
  const Result<std::string> sid = current_user_sid();
  ASSERT_TRUE(sid.has_value());
  EXPECT_TRUE(sid->starts_with("S-1-5-"));
  EXPECT_TRUE(check_sid(*sid).has_value());
}

TEST(PipeSecurityTest, ChecksSids) {
  EXPECT_TRUE(check_sid(system_sid).has_value());
  EXPECT_TRUE(check_sid(owner).has_value());
  for (const std::string_view bad : {"", "S-1", "owner", "S-1-5-18)(A;;FA;;;WD"}) {
    const Result<void> checked = check_sid(bad);
    ASSERT_FALSE(checked.has_value()) << bad;
    EXPECT_EQ(checked.error().code, Errc::invalid_argument);
  }
}

TEST(PipeSecurityTest, RefusesADescriptorWithoutDacl) {
  Result<wil::unique_hlocal_security_descriptor> descriptor = make_security_descriptor("O:SY");
  ASSERT_TRUE(descriptor.has_value());
  const Result<std::vector<AllowedAce>> aces = allowed_aces(descriptor->get());
  ASSERT_FALSE(aces.has_value());
  EXPECT_EQ(aces.error().code, Errc::invalid_data);
}

}  // namespace
}  // namespace myclean::platform
