#include "core/resident/sampler.hpp"

#include <chrono>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/units.hpp"
#include "resident_fixtures.hpp"

namespace myclean::resident {
namespace {

using namespace std::chrono_literals;
using namespace myclean::literals;
using testing::noon;
using testing::view_of;

TEST(TreeKeyTest, IsTheLowerCaseFileNameOfTheImage) {
  EXPECT_EQ(tree_key("ArmouryCrate.Service.exe"), "armourycrate.service.exe");
  EXPECT_EQ(tree_key(R"(C:\Program Files\ASUS\ArmouryCrate.exe)"), "armourycrate.exe");
  EXPECT_EQ(tree_key("C:/tools/Agent.EXE"), "agent.exe");
}

TEST(UsageSamplerTest, CountsWhatEachTreeUsedSinceTheFirstTable) {
  UsageSampler sampler;
  sampler.observe(view_of(noon, {{{.pid = 10, .image = "Steam.exe", .cpu = 100s, .io_bytes = 1000},
                                  {.pid = 11, .image = "steamwebhelper.exe", .cpu = 50s}}}));
  // The first table is the baseline: nothing counted yet, memory as it is.
  const std::vector<MinuteUse> first = sampler.take();
  ASSERT_EQ(first.size(), 1U);
  EXPECT_EQ(first[0].key, "steam.exe");
  EXPECT_DOUBLE_EQ(first[0].cpu_seconds, 0.0);
  EXPECT_EQ(first[0].disk_bytes, 0U);

  sampler.observe(view_of(noon + 1s, {{{.pid = 10,
                                        .image = "Steam.exe",
                                        .cpu = 102s,
                                        .io_bytes = 1500,
                                        .private_bytes = 300_MiB,
                                        .ram = 200_MiB},
                                       {.pid = 11,
                                        .image = "steamwebhelper.exe",
                                        .cpu = 51s,
                                        .private_bytes = 100_MiB,
                                        .ram = 50_MiB}}}));
  sampler.observe(view_of(noon + 2s, {{{.pid = 10,
                                        .image = "Steam.exe",
                                        .cpu = 103s,
                                        .io_bytes = 1600,
                                        .private_bytes = 310_MiB,
                                        .ram = 210_MiB},
                                       {.pid = 11,
                                        .image = "steamwebhelper.exe",
                                        .cpu = 51s,
                                        .private_bytes = 100_MiB,
                                        .ram = 50_MiB},
                                       // Started after the previous table: counts from its start.
                                       {.pid = 12,
                                        .image = "steamwebhelper.exe",
                                        .cpu = 4s,
                                        .io_bytes = 70,
                                        .private_bytes = 40_MiB,
                                        .ram = 30_MiB,
                                        .created = noon + 1500ms}}}));
  const std::vector<MinuteUse> second = sampler.take();
  ASSERT_EQ(second.size(), 1U);
  EXPECT_TRUE(second[0].running);
  EXPECT_DOUBLE_EQ(second[0].cpu_seconds, 3.0 + 1.0 + 4.0);
  EXPECT_EQ(second[0].disk_bytes, 600U + 70U);
  // Memory of the latest table.
  EXPECT_EQ(second[0].private_bytes, 450_MiB);
  EXPECT_EQ(second[0].ram, 290_MiB);
}

TEST(UsageSamplerTest, AddsTreesOfOneProgramAndSkipsTheIdleProcess) {
  UsageSampler sampler;
  sampler.observe(view_of(noon, {{{.pid = 0, .image = "System Idle Process", .cpu = 9000s}},
                                 {{.pid = 20, .image = "svchost.exe", .private_bytes = 10_MiB}},
                                 {{.pid = 21, .image = "SvcHost.exe", .private_bytes = 30_MiB}}}));
  const std::vector<MinuteUse> use = sampler.take();
  ASSERT_EQ(use.size(), 1U);
  EXPECT_EQ(use[0].key, "svchost.exe");
  EXPECT_EQ(use[0].private_bytes, 40_MiB);
}

TEST(UsageSamplerTest, AProgramGoneSinceKeepsItsCpuButIsNotRunning) {
  UsageSampler sampler;
  sampler.observe(view_of(noon, {{{.pid = 30, .image = "build.exe", .cpu = 1s}},
                                 {{.pid = 31, .image = "tray.exe", .cpu = 1s}}}));
  static_cast<void>(sampler.take());
  sampler.observe(view_of(noon + 1s, {{{.pid = 30, .image = "build.exe", .cpu = 6s}},
                                      {{.pid = 31, .image = "tray.exe", .cpu = 1s}}}));
  sampler.observe(view_of(noon + 2s, {{{.pid = 31, .image = "tray.exe", .cpu = 1s}}}));
  const std::vector<MinuteUse> use = sampler.take();
  ASSERT_EQ(use.size(), 2U);
  EXPECT_EQ(use[0].key, "build.exe");
  EXPECT_FALSE(use[0].running);
  EXPECT_DOUBLE_EQ(use[0].cpu_seconds, 5.0);
  EXPECT_EQ(use[0].private_bytes, 0_B);
  EXPECT_EQ(use[1].key, "tray.exe");
  EXPECT_TRUE(use[1].running);
}

TEST(UsageSamplerTest, AttributesNetworkBytesToTheTreeOfEachProcess) {
  UsageSampler sampler;
  const WallTime helper_start = noon - 1h;
  sampler.observe(
      view_of(noon, {{{.pid = 40, .image = "Discord.exe"},
                      {.pid = 41, .image = "Discord.exe", .created = helper_start + 400ms}}}));
  std::vector<MinuteUse> use = sampler.take();
  const std::vector<ProcessTraffic> traffic = {
      // The network history keeps whole seconds.
      {.pid = 41, .created = helper_start, .image = "Discord.exe", .bytes = 5000},
      // Not in any table: its own image.
      {.pid = 50, .created = noon - 30s, .image = R"(C:\Tools\Updater.exe)", .bytes = 700},
      // Unknown process: left out.
      {.pid = 51, .bytes = 900},
      // Known by PID only.
      {.pid = 40, .bytes = 10}};
  sampler.attribute(traffic, use);
  ASSERT_EQ(use.size(), 2U);
  EXPECT_EQ(use[0].key, "discord.exe");
  EXPECT_EQ(use[0].net_bytes, 5010U);
  EXPECT_EQ(use[1].key, "updater.exe");
  EXPECT_FALSE(use[1].running);
  EXPECT_EQ(use[1].net_bytes, 700U);
}

TEST(UsageSamplerTest, ForgetsProcessesAfterAWhile) {
  UsageSampler sampler;
  sampler.observe(view_of(noon, {{{.pid = 60, .image = "Game.exe"}}}));
  static_cast<void>(sampler.take());
  sampler.observe(view_of(noon + UsageSampler::remember + 1s, {}));
  std::vector<MinuteUse> use = sampler.take();
  sampler.attribute(std::vector<ProcessTraffic>{{.pid = 60, .created = noon - 24h, .bytes = 1}},
                    use);
  EXPECT_TRUE(use.empty());
}

}  // namespace
}  // namespace myclean::resident
