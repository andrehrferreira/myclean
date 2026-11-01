#include "core/governor/pressure.hpp"

#include <chrono>
#include <vector>

#include <gtest/gtest.h>

#include "core/governor/policy.hpp"
#include "core/governor/reason.hpp"
#include "core/model/units.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::governor {
namespace {

using namespace myclean::literals;
using std::chrono::seconds;

// 150 GiB commit limit, as in the spec's commit scenario.
constexpr Bytes commit_limit = 150_GiB;

class PressureTest : public ::testing::Test {
 protected:
  // A calm machine: every signal well inside its exit threshold.
  [[nodiscard]] PressureSample calm() const {
    return {.time = clock_.monotonic(),
            .cpu_percent = 30.0,
            .available = 20_GiB,
            .commit = 40_GiB,
            .commit_limit = commit_limit,
            .hard_faults_per_second = 10.0};
  }

  // Advances one second, then evaluates `sample` taken at that second.
  const PressureStatus& next(PressureSample sample, bool game_mode = false) {
    clock_.advance(seconds{1});
    sample.time = clock_.monotonic();
    return detector_.update(sample, game_mode);
  }

  const PressureStatus& cpu(double percent, bool game_mode = false) {
    PressureSample sample = calm();
    sample.cpu_percent = percent;
    return next(sample, game_mode);
  }

  // Seconds of `percent` CPU; returns the status after the last one.
  PressureStatus cpu_for(int count, double percent, bool game_mode = false) {
    PressureStatus status;
    for (int second = 0; second < count; ++second) {
      status = cpu(percent, game_mode);
    }
    return status;
  }

  [[nodiscard]] const PressureStatus& status() const { return detector_.status(); }

 private:
  fakes::FakeClock clock_{WallTime{}};
  PressureDetector detector_{PressurePolicy{}};
};

TEST_F(PressureTest, SustainedCpuEntersPressure) {
  const PressureStatus before = cpu_for(14, 95.0);
  EXPECT_EQ(before.state, PressureState::normal);
  const PressureStatus& after = cpu(95.0);
  EXPECT_EQ(after.state, PressureState::pressure);
  EXPECT_EQ(after.reasons, std::vector<Reason>{Reason::cpu});
  EXPECT_FALSE(after.clearing);
}

TEST_F(PressureTest, ShortCpuSpikeIgnored) {
  EXPECT_EQ(cpu_for(10, 95.0).state, PressureState::normal);
  EXPECT_EQ(cpu_for(30, 60.0).state, PressureState::normal);
  // The spike's run is over: 14 more seconds above 90 % are not enough.
  EXPECT_EQ(cpu_for(14, 95.0).state, PressureState::normal);
}

TEST_F(PressureTest, ExactlyNinetyPercentIsNotAbove) {
  EXPECT_EQ(cpu_for(30, 90.0).state, PressureState::normal);
}

TEST_F(PressureTest, LowAvailableMemoryEntersPressure) {
  PressureSample sample = calm();
  sample.available = Bytes{(15ULL << 30U) / 2};  // 7.5 GiB
  const PressureStatus& status = next(sample);
  EXPECT_EQ(status.state, PressureState::pressure);
  EXPECT_EQ(status.reasons, std::vector<Reason>{Reason::available_memory});
}

TEST_F(PressureTest, CommitNearLimitEntersPressure) {
  PressureSample sample = calm();
  sample.commit = 136_GiB;  // 90.7 % of 150 GiB
  const PressureStatus& status = next(sample);
  EXPECT_EQ(status.state, PressureState::pressure);
  EXPECT_EQ(status.reasons, std::vector<Reason>{Reason::commit});
}

TEST_F(PressureTest, CommitAtNinetyPercentIsNotAbove) {
  PressureSample sample = calm();
  sample.commit = 135_GiB;  // exactly 90 %
  EXPECT_EQ(next(sample).state, PressureState::normal);
}

TEST_F(PressureTest, SustainedHardFaultsEnterPressure) {
  PressureSample sample = calm();
  sample.hard_faults_per_second = 1500.0;
  for (int second = 0; second < 14; ++second) {
    EXPECT_EQ(next(sample).state, PressureState::normal);
  }
  const PressureStatus& status = next(sample);
  EXPECT_EQ(status.state, PressureState::pressure);
  EXPECT_EQ(status.reasons, std::vector<Reason>{Reason::hard_faults});
}

TEST_F(PressureTest, GameModeShortensTheDwell) {
  EXPECT_EQ(cpu_for(4, 95.0, true).state, PressureState::normal);
  const PressureStatus& status = cpu(95.0, true);
  EXPECT_EQ(status.state, PressureState::pressure);
  EXPECT_EQ(status.reasons, std::vector<Reason>{Reason::cpu});
}

TEST_F(PressureTest, ReasonsListEverySignalPastItsEntryThreshold) {
  cpu_for(14, 95.0);
  PressureSample sample = calm();
  sample.cpu_percent = 95.0;
  sample.available = 4_GiB;
  sample.commit = 140_GiB;
  const PressureStatus& status = next(sample);
  EXPECT_EQ(status.reasons,
            (std::vector<Reason>{Reason::cpu, Reason::available_memory, Reason::commit}));
}

TEST_F(PressureTest, PressureClearsAfter60s) {
  cpu_for(15, 95.0);
  ASSERT_EQ(status().state, PressureState::pressure);
  for (int second = 1; second < 60; ++second) {
    const PressureStatus& status = cpu(50.0);
    ASSERT_EQ(status.state, PressureState::pressure) << second;
    EXPECT_TRUE(status.clearing);
    EXPECT_EQ(status.reasons, std::vector<Reason>{Reason::cpu});  // the last reasons it had
  }
  const PressureStatus& cleared = cpu(50.0);
  EXPECT_EQ(cleared.state, PressureState::normal);
  EXPECT_TRUE(cleared.reasons.empty());
  EXPECT_FALSE(cleared.clearing);
}

TEST_F(PressureTest, RelapseRestartsTheClearTimer) {
  cpu_for(15, 95.0);
  EXPECT_TRUE(cpu_for(45, 50.0).clearing);
  const PressureStatus relapse = cpu_for(15, 95.0);
  EXPECT_EQ(relapse.state, PressureState::pressure);
  EXPECT_FALSE(relapse.clearing);
  // The timer starts again from zero once every signal is back below its exit threshold.
  EXPECT_EQ(cpu_for(59, 50.0).state, PressureState::pressure);
  EXPECT_EQ(cpu(50.0).state, PressureState::normal);
}

TEST_F(PressureTest, SignalBetweenExitAndEntryHoldsPressure) {
  cpu_for(15, 95.0);
  EXPECT_FALSE(cpu_for(120, 85.0).clearing);
  EXPECT_EQ(status().state, PressureState::pressure);
}

TEST_F(PressureTest, MemoryMustRecoverToItsExitThreshold) {
  PressureSample low = calm();
  low.available = 7_GiB;
  next(low);
  PressureSample recovering = calm();
  recovering.available = 9_GiB;  // above the entry, below the exit threshold
  for (int second = 0; second < 90; ++second) {
    EXPECT_FALSE(next(recovering).clearing);
  }
  EXPECT_EQ(status().state, PressureState::pressure);
}

TEST_F(PressureTest, HardFaultExitIsHalfTheEntry) {
  PressureSample faults = calm();
  faults.hard_faults_per_second = 1500.0;
  for (int second = 0; second < 15; ++second) {
    next(faults);
  }
  faults.hard_faults_per_second = 600.0;  // below the entry, above half of it
  EXPECT_FALSE(next(faults).clearing);
  faults.hard_faults_per_second = 500.0;
  EXPECT_TRUE(next(faults).clearing);
}

TEST(PressureStateText, RoundTrips) {
  EXPECT_EQ(to_string(PressureState::pressure), "pressure");
  EXPECT_EQ(parse_pressure_state("normal"), PressureState::normal);
  EXPECT_FALSE(parse_pressure_state("busy").has_value());
}

TEST(ReasonText, RoundTripsEveryReason) {
  for (int value = 0; value <= static_cast<int>(Reason::policy); ++value) {
    const auto reason = static_cast<Reason>(value);
    EXPECT_EQ(parse_reason(to_string(reason)), reason);
  }
  EXPECT_EQ(to_string(Reason::protected_process), "protected");
  EXPECT_EQ(to_string(Reason::orphaned_suspend), "orphaned_suspend");
  EXPECT_TRUE(is_pressure_signal(Reason::hard_faults));
  EXPECT_FALSE(is_pressure_signal(Reason::pause));
}

}  // namespace
}  // namespace myclean::governor
