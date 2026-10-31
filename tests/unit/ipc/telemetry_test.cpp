#include "core/ipc/telemetry.hpp"

#include <gtest/gtest.h>

#include "core/ipc/json_rpc.hpp"

namespace myclean::ipc {
namespace {

TEST(EntityTokenTest, TurnsEverySeriesEntityIntoAToken) {
  EXPECT_EQ(entity_token("system"), "system");
  EXPECT_EQ(entity_token("7"), "7");
  EXPECT_EQ(entity_token("C:"), "C");
  EXPECT_EQ(entity_token(R"(D:\mnt\my-data)"), "D_mnt_my-data");
  EXPECT_EQ(entity_token("0,17"), "0_17");
  EXPECT_EQ(entity_token("luid_0x00000000_0x0000D1B5_phys_0/Video Decode"),
            "luid_0x00000000_0x0000D1B5_phys_0_Video_Decode");
  for (const char* entity : {"C:", R"(D:\mnt\data)", "0,17", "luid_0x0_0x1_phys_0/3D"}) {
    EXPECT_TRUE(is_token(entity_token(entity))) << entity;
  }
}

}  // namespace
}  // namespace myclean::ipc
