#include "core/governor/policy.hpp"

#include <chrono>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/rules/data_files.hpp"
#include "core/rules/schema.hpp"

namespace myclean::governor {
namespace {

using namespace myclean::literals;
using std::chrono::minutes;
using std::chrono::seconds;

Result<GovernorPolicy> parse(const std::string& text) {
  return parse_policy_file(text, "governor.toml");
}

// The message of a rejected file, or a failure.
std::string error_of(const std::string& text) {
  const Result<GovernorPolicy> policy = parse(text);
  if (policy) {
    ADD_FAILURE() << "accepted:\n" << text;
    return {};
  }
  EXPECT_EQ(policy.error().code, Errc::invalid_data);
  return policy.error().message;
}

TEST(GovernorPolicy, DefaultsAreTheSpecValues) {
  const GovernorPolicy policy;
  EXPECT_EQ(policy.mode, GovernorMode::observe);
  EXPECT_EQ(policy.pressure.cpu_enter_percent, 90.0);
  EXPECT_EQ(policy.pressure.cpu_dwell, seconds{15});
  EXPECT_EQ(policy.pressure.available_enter, 8_GiB);
  EXPECT_EQ(policy.pressure.commit_enter_percent, 90.0);
  EXPECT_EQ(policy.pressure.hard_faults_enter, 1000.0);
  EXPECT_EQ(policy.pressure.hard_faults_dwell, seconds{15});
  EXPECT_EQ(policy.pressure.cpu_exit_percent, 80.0);
  EXPECT_EQ(policy.pressure.available_exit, 10_GiB);
  EXPECT_EQ(policy.pressure.commit_exit_percent, 85.0);
  EXPECT_EQ(policy.pressure.hard_faults_exit, 500.0);
  EXPECT_EQ(policy.pressure.clear_after, seconds{60});
  EXPECT_EQ(policy.pressure.game_dwell, seconds{5});
  EXPECT_EQ(policy.ladder.cooldown, seconds{10});
  EXPECT_EQ(policy.ladder.game_cooldown, seconds{5});
  EXPECT_EQ(policy.ladder.cap_cpu_percent, 50);
  EXPECT_EQ(policy.ladder.cap_commit_headroom_percent, 25);
  EXPECT_EQ(policy.ladder.cap_commit_min_headroom, 64_MiB);
  EXPECT_EQ(policy.ladder.resume_blocks_suspend, minutes{10});
  EXPECT_TRUE(policy.ladder.kill.empty());
  EXPECT_EQ(policy.ladder.notification_interval, minutes{5});
  EXPECT_EQ(policy.jobs.build.cpu_percent, 75);
  EXPECT_EQ(policy.jobs.build.commit, 96_GiB);
  EXPECT_EQ(policy.jobs.test.cpu_percent, 75);
  EXPECT_EQ(policy.jobs.test.commit, 64_GiB);
  EXPECT_EQ(policy.jobs.game_build_cpu_percent, 40);
  EXPECT_EQ(policy.jobs.notification_percent, 80);
  EXPECT_EQ(policy.jobs.rearm_percent, 70);
  for (const rules::ProcessClass process_class :
       {rules::ProcessClass::build, rules::ProcessClass::test, rules::ProcessClass::normal}) {
    EXPECT_FALSE(kill_enabled(policy.ladder, process_class));
  }
}

TEST(GovernorPolicy, ShippedFileHoldsTheDefaults) {
  const Result<GovernorPolicy> loaded =
      load_policy(rules::DiskDataFiles{}, std::filesystem::path{MYCLEAN_DATA_DIR});
  ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
  const GovernorPolicy defaults;
  EXPECT_EQ(loaded->mode, defaults.mode);
  EXPECT_EQ(loaded->pressure.cpu_enter_percent, defaults.pressure.cpu_enter_percent);
  EXPECT_EQ(loaded->pressure.cpu_dwell, defaults.pressure.cpu_dwell);
  EXPECT_EQ(loaded->pressure.available_enter, defaults.pressure.available_enter);
  EXPECT_EQ(loaded->pressure.commit_enter_percent, defaults.pressure.commit_enter_percent);
  EXPECT_EQ(loaded->pressure.hard_faults_enter, defaults.pressure.hard_faults_enter);
  EXPECT_EQ(loaded->pressure.hard_faults_dwell, defaults.pressure.hard_faults_dwell);
  EXPECT_EQ(loaded->pressure.cpu_exit_percent, defaults.pressure.cpu_exit_percent);
  EXPECT_EQ(loaded->pressure.available_exit, defaults.pressure.available_exit);
  EXPECT_EQ(loaded->pressure.commit_exit_percent, defaults.pressure.commit_exit_percent);
  EXPECT_EQ(loaded->pressure.hard_faults_exit, defaults.pressure.hard_faults_exit);
  EXPECT_EQ(loaded->pressure.clear_after, defaults.pressure.clear_after);
  EXPECT_EQ(loaded->pressure.game_dwell, defaults.pressure.game_dwell);
  EXPECT_EQ(loaded->ladder.cooldown, defaults.ladder.cooldown);
  EXPECT_EQ(loaded->ladder.game_cooldown, defaults.ladder.game_cooldown);
  EXPECT_EQ(loaded->ladder.cap_cpu_percent, defaults.ladder.cap_cpu_percent);
  EXPECT_EQ(loaded->ladder.cap_commit_headroom_percent,
            defaults.ladder.cap_commit_headroom_percent);
  EXPECT_EQ(loaded->ladder.cap_commit_min_headroom, defaults.ladder.cap_commit_min_headroom);
  EXPECT_EQ(loaded->ladder.resume_blocks_suspend, defaults.ladder.resume_blocks_suspend);
  EXPECT_EQ(loaded->ladder.kill, defaults.ladder.kill);
  EXPECT_EQ(loaded->ladder.notification_interval, defaults.ladder.notification_interval);
  EXPECT_EQ(loaded->jobs.build.cpu_percent, defaults.jobs.build.cpu_percent);
  EXPECT_EQ(loaded->jobs.build.commit, defaults.jobs.build.commit);
  EXPECT_EQ(loaded->jobs.test.cpu_percent, defaults.jobs.test.cpu_percent);
  EXPECT_EQ(loaded->jobs.test.commit, defaults.jobs.test.commit);
  EXPECT_EQ(loaded->jobs.game_build_cpu_percent, defaults.jobs.game_build_cpu_percent);
  EXPECT_EQ(loaded->jobs.notification_percent, defaults.jobs.notification_percent);
  EXPECT_EQ(loaded->jobs.rearm_percent, defaults.jobs.rearm_percent);
  EXPECT_EQ(loaded->orphans.enabled, defaults.orphans.enabled);
  EXPECT_EQ(loaded->orphans.min_trees, defaults.orphans.min_trees);
  EXPECT_EQ(loaded->orphans.min_age, defaults.orphans.min_age);
}

TEST(GovernorPolicy, OrphanedLoops) {
  const GovernorPolicy defaults;
  EXPECT_TRUE(defaults.orphans.enabled);
  EXPECT_EQ(defaults.orphans.min_trees, 3);
  EXPECT_EQ(defaults.orphans.min_age, minutes{10});
  const Result<GovernorPolicy> read =
      parse("[orphans]\nenabled = false\nmin_trees = 5\nmin_age_minutes = 30\n");
  ASSERT_TRUE(read.has_value()) << read.error().message;
  EXPECT_FALSE(read->orphans.enabled);
  EXPECT_EQ(read->orphans.min_trees, 5);
  EXPECT_EQ(read->orphans.min_age, minutes{30});
  EXPECT_NE(error_of("[orphans]\nmin_trees = 1\n").find("'orphans.min_trees'"), std::string::npos);
  EXPECT_NE(error_of("[orphans]\nenabled = 1\n").find("true or false"), std::string::npos);
  EXPECT_NE(error_of("[orphans]\nmin_age_minutes = 0\n").find("'orphans.min_age_minutes'"),
            std::string::npos);
}

TEST(GovernorPolicy, EmptyFileIsTheDefaults) {
  const Result<GovernorPolicy> policy = parse("");
  ASSERT_TRUE(policy.has_value()) << policy.error().message;
  EXPECT_EQ(policy->pressure.cpu_dwell, seconds{15});
  EXPECT_EQ(policy->jobs.build.commit, 96_GiB);
}

TEST(GovernorPolicy, ModeIsObserveOrAct) {
  const Result<GovernorPolicy> act = parse(R"([governor]
mode = "act")");
  ASSERT_TRUE(act.has_value()) << act.error().message;
  EXPECT_EQ(act->mode, GovernorMode::act);
  const Result<GovernorPolicy> observe = parse(R"([governor]
mode = "observe")");
  ASSERT_TRUE(observe.has_value()) << observe.error().message;
  EXPECT_EQ(observe->mode, GovernorMode::observe);
  EXPECT_EQ(parse("[governor]")->mode, GovernorMode::observe);

  EXPECT_NE(error_of(R"([governor]
mode = "enforce")")
                .find("governor.mode"),
            std::string::npos);
  EXPECT_NE(error_of(R"([governor]
mode = 1)")
                .find("governor.mode"),
            std::string::npos);
  EXPECT_NE(error_of(R"([governor]
level = "act")")
                .find("governor.level"),
            std::string::npos);
  EXPECT_EQ(to_string(GovernorMode::act), "act");
  EXPECT_EQ(parse_governor_mode("observe"), GovernorMode::observe);
  EXPECT_FALSE(parse_governor_mode("Act").has_value());
}

TEST(GovernorPolicy, ReadsEveryKey) {
  const Result<GovernorPolicy> policy = parse(R"(
[pressure]
cpu_percent = 95
cpu_seconds = 20
available_gib = 7.5
commit_percent = 92
hard_faults_per_second = 2000
hard_faults_seconds = 30
[clear]
cpu_percent = 70
available_gib = 12
commit_percent = 80
seconds = 120
[ladder]
cooldown_seconds = 20
cap_cpu_percent = 30
cap_commit_headroom_percent = 50
cap_commit_min_headroom_mib = 128
resume_blocks_suspend_minutes = 15
kill = ["default", "build"]
[game_mode]
pressure_seconds = 3
cooldown_seconds = 4
build_cpu_percent = 25
[jobs]
notification_percent = 90
rearm_percent = 60
[jobs.build]
cpu_percent = 60
commit_gib = 48
[jobs.test]
cpu_percent = 50
commit_gib = 32
[notifications]
interval_minutes = 10
)");
  ASSERT_TRUE(policy.has_value()) << policy.error().message;
  EXPECT_EQ(policy->pressure.cpu_enter_percent, 95.0);
  EXPECT_EQ(policy->pressure.cpu_dwell, seconds{20});
  EXPECT_EQ(policy->pressure.available_enter, Bytes{(15ULL << 30U) / 2});
  EXPECT_EQ(policy->pressure.commit_enter_percent, 92.0);
  EXPECT_EQ(policy->pressure.hard_faults_enter, 2000.0);
  EXPECT_EQ(policy->pressure.hard_faults_dwell, seconds{30});
  EXPECT_EQ(policy->pressure.cpu_exit_percent, 70.0);
  EXPECT_EQ(policy->pressure.available_exit, 12_GiB);
  EXPECT_EQ(policy->pressure.commit_exit_percent, 80.0);
  EXPECT_EQ(policy->pressure.hard_faults_exit, 1000.0);  // half the entry when not given
  EXPECT_EQ(policy->pressure.clear_after, seconds{120});
  EXPECT_EQ(policy->pressure.game_dwell, seconds{3});
  EXPECT_EQ(policy->ladder.cooldown, seconds{20});
  EXPECT_EQ(policy->ladder.game_cooldown, seconds{4});
  EXPECT_EQ(policy->ladder.cap_cpu_percent, 30);
  EXPECT_EQ(policy->ladder.cap_commit_headroom_percent, 50);
  EXPECT_EQ(policy->ladder.cap_commit_min_headroom, 128_MiB);
  EXPECT_EQ(policy->ladder.resume_blocks_suspend, minutes{15});
  EXPECT_TRUE(kill_enabled(policy->ladder, rules::ProcessClass::normal));
  EXPECT_TRUE(kill_enabled(policy->ladder, rules::ProcessClass::build));
  EXPECT_FALSE(kill_enabled(policy->ladder, rules::ProcessClass::test));
  EXPECT_EQ(policy->jobs.game_build_cpu_percent, 25);
  EXPECT_EQ(policy->jobs.notification_percent, 90);
  EXPECT_EQ(policy->jobs.rearm_percent, 60);
  EXPECT_EQ(policy->jobs.build.cpu_percent, 60);
  EXPECT_EQ(policy->jobs.build.commit, 48_GiB);
  EXPECT_EQ(policy->jobs.test.cpu_percent, 50);
  EXPECT_EQ(policy->jobs.test.commit, 32_GiB);
  EXPECT_EQ(policy->ladder.notification_interval, minutes{10});
}

TEST(GovernorPolicy, ExplicitHardFaultExitWins) {
  const Result<GovernorPolicy> policy =
      parse("[pressure]\nhard_faults_per_second = 1000\n[clear]\nhard_faults_per_second = 200\n");
  ASSERT_TRUE(policy.has_value()) << policy.error().message;
  EXPECT_EQ(policy->pressure.hard_faults_exit, 200.0);
}

TEST(GovernorPolicy, MalformedFileIsRejectedWholeNamingFileAndField) {
  const std::string message = error_of("[pressure]\ncpu_percent = \"high\"\n");
  EXPECT_NE(message.find("governor.toml"), std::string::npos) << message;
  EXPECT_NE(message.find("'pressure.cpu_percent'"), std::string::npos) << message;
}

TEST(GovernorPolicy, RejectsInvalidValues) {
  EXPECT_NE(error_of("[pressure]\ncpu_percent = 150\n").find("'pressure.cpu_percent'"),
            std::string::npos);
  EXPECT_NE(error_of("[ladder]\ncooldown_seconds = 0\n").find("'ladder.cooldown_seconds'"),
            std::string::npos);
  EXPECT_NE(error_of("[ladder]\ncooldown_seconds = 2.5\n").find("whole number"), std::string::npos);
  EXPECT_NE(error_of("[ladder]\nkill = [\"system\"]\n").find("'ladder.kill'"), std::string::npos);
  EXPECT_NE(error_of("[ladder]\nkill = [\"games\"]\n").find("'ladder.kill'"), std::string::npos);
  EXPECT_NE(error_of("[ladder]\ncap_commit_min_headroom_mib = 16\n")
                .find("'ladder.cap_commit_min_headroom_mib'"),
            std::string::npos);
  EXPECT_NE(error_of("[jobs.build]\ncommit_gib = 0\n").find("'jobs.build.commit_gib'"),
            std::string::npos);
  EXPECT_NE(error_of("[jobs]\nrearm_percent = 85\n").find("'jobs.rearm_percent'"),
            std::string::npos);
  EXPECT_NE(error_of("[pressure]\ncpu_typo = 1\n").find("'pressure.cpu_typo' is not a known"),
            std::string::npos);
  EXPECT_NE(error_of("unknown = 1\n").find("'unknown'"), std::string::npos);
  EXPECT_NE(error_of("pressure = 3\n").find("must be a table"), std::string::npos);
  EXPECT_NE(error_of("[pressure\n").find("is not valid TOML"), std::string::npos);
}

TEST(GovernorPolicy, ExitThresholdsMustNotBeStricterThanEntry) {
  EXPECT_NE(error_of("[clear]\ncpu_percent = 95\n").find("'clear.cpu_percent'"), std::string::npos);
  EXPECT_NE(error_of("[clear]\navailable_gib = 4\n").find("'clear.available_gib'"),
            std::string::npos);
  EXPECT_NE(error_of("[clear]\ncommit_percent = 95\n").find("'clear.commit_percent'"),
            std::string::npos);
  EXPECT_NE(
      error_of("[clear]\nhard_faults_per_second = 5000\n").find("'clear.hard_faults_per_second'"),
      std::string::npos);
}

}  // namespace
}  // namespace myclean::governor
