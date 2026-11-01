#include "core/governor/game_mode.hpp"

#include <gtest/gtest.h>

#include "core/governor/classifier.hpp"
#include "core/governor/jobs.hpp"
#include "core/governor/policy.hpp"
#include "core/rules/schema.hpp"

namespace myclean::governor {
namespace {

using rules::ProcessClass;

TEST(GameMode, GameStartsDuringABuild) {
  const JobPolicy policy;
  EXPECT_EQ(decide_game_mode(GameModeOverride::automatic, false, policy).build_cpu_percent, 75);
  const GameMode playing = decide_game_mode(GameModeOverride::automatic, true, policy);
  EXPECT_TRUE(playing.active);
  EXPECT_EQ(playing.build_cpu_percent, 40);
  EXPECT_EQ(class_job_limits(ProcessClass::build, policy, playing.active)
                .value_or(JobLimits{})
                .cpu_percent,
            40);
  // The last game exits.
  const GameMode after = decide_game_mode(GameModeOverride::automatic, false, policy);
  EXPECT_FALSE(after.active);
  EXPECT_EQ(after.build_cpu_percent, 75);
  EXPECT_EQ(
      class_job_limits(ProcessClass::build, policy, after.active).value_or(JobLimits{}).cpu_percent,
      75);
}

TEST(GameMode, OverrideOff) {
  const GameMode mode = decide_game_mode(GameModeOverride::off, true, JobPolicy{});
  EXPECT_FALSE(mode.active);
  EXPECT_EQ(mode.build_cpu_percent, 75);
  // The detected game is still class game, so it is protected.
  EXPECT_EQ(final_class(ProcessClass::normal, true), ProcessClass::game);
}

TEST(GameMode, OverrideOn) {
  const GameMode mode = decide_game_mode(GameModeOverride::on, false, JobPolicy{});
  EXPECT_TRUE(mode.active);
  EXPECT_EQ(mode.build_cpu_percent, 40);
}

TEST(GameMode, OverrideText) {
  EXPECT_EQ(to_string(GameModeOverride::automatic), "auto");
  EXPECT_EQ(parse_game_mode_override("off"), GameModeOverride::off);
  EXPECT_FALSE(parse_game_mode_override("automatic").has_value());
}

}  // namespace
}  // namespace myclean::governor
