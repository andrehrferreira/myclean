#include "core/games/fullscreen.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/perf_counters.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::games {
namespace {

using std::chrono::seconds;
using telemetry::CounterValue;

constexpr std::string_view dgpu = "luid_0x00000000_0x02135831";
constexpr std::string_view basic_render = "luid_0x00000000_0x0001DEDF";

CounterValue engine(std::uint32_t pid, std::string_view luid, int index, std::string_view type,
                    double percent) {
  return CounterValue{.instance = "pid_" + std::to_string(pid) + "_" + std::string(luid) +
                                  "_phys_0_eng_" + std::to_string(index) + "_engtype_" +
                                  std::string(type),
                      .value = percent};
}

TEST(Gpu3dByPid, SumsPerAdapterAndTakesTheBusiestAdapter) {
  const std::vector<CounterValue> values = {
      engine(100932, dgpu, 0, "3D", 9.0),
      engine(100932, dgpu, 1, "3D", 6.5),
      engine(100932, dgpu, 2, "High Priority 3D", 40.0),  // another engine type
      engine(100932, dgpu, 3, "Video Decode", 30.0),
      engine(100932, basic_render, 0, "3D", 1.0),
      engine(100932, basic_render, 1, "3D", 1.0),
      engine(4120, basic_render, 0, "3D", 0.0),
      engine(77, dgpu, 0, "3D", 80.0),
      engine(77, dgpu, 1, "3D", 70.0),
      engine(55, dgpu, 0, "Copy", 5.0),
      CounterValue{.instance = "pid_x_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .value = 50.0},
      CounterValue{.instance = "pid_9_eng_0_engtype_3D", .value = 50.0},
      CounterValue{.instance = "pid_99999999999_luid_0x0_0x1_phys_0_eng_0_engtype_3D",
                   .value = 50.0},
  };
  const std::map<std::uint32_t, double> expected = {{4120, 0.0}, {77, 100.0}, {100932, 15.5}};
  EXPECT_EQ(gpu_3d_by_pid(values), expected);
}

// Counters whose engine values a test sets; the first collect of a query has no values, as with
// PDH rate counters.
class FakeCounters final : public telemetry::IPerfCounters {
 public:
  [[nodiscard]] Result<std::unique_ptr<telemetry::ICounterQuery>> open() override {
    return std::make_unique<Query>(*this);
  }

  std::vector<CounterValue> values;
  bool missing = false;

 private:
  class Query final : public telemetry::ICounterQuery {
   public:
    explicit Query(FakeCounters& owner) : owner_(&owner) {}

    [[nodiscard]] Result<std::size_t> add(std::string_view path) override {
      if (owner_->missing || path != gpu_engine_counter) {
        return fail(Errc::not_found, "no such counter");
      }
      return std::size_t{0};
    }
    [[nodiscard]] Result<void> collect() override {
      ++collects_;
      return {};
    }
    [[nodiscard]] Result<std::vector<CounterValue>> values(std::size_t /*index*/) const override {
      return collects_ < 2 ? std::vector<CounterValue>{} : owner_->values;
    }

   private:
    FakeCounters* owner_;
    int collects_ = 0;
  };
};

TEST(Gpu3dSampler, SamplesTheEngineCounter) {
  FakeCounters counters;
  counters.values = {engine(42, dgpu, 0, "3D", 12.0)};
  Result<Gpu3dSampler> sampler = Gpu3dSampler::open(counters);
  ASSERT_TRUE(sampler) << sampler.error().message;
  const Result<std::map<std::uint32_t, double>> first = sampler->sample();
  ASSERT_TRUE(first);
  EXPECT_TRUE(first->empty());
  const Result<std::map<std::uint32_t, double>> second = sampler->sample();
  ASSERT_TRUE(second);
  EXPECT_EQ(*second, (std::map<std::uint32_t, double>{{42, 12.0}}));

  counters.missing = true;
  const Result<Gpu3dSampler> none = Gpu3dSampler::open(counters);
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error().code, Errc::not_found);
  EXPECT_TRUE(none.error().message.starts_with(R"(\GPU Engine(*)\Utilization Percentage: )"));
}

TEST(NotificationStates, HaveFixedNamesAndTwoFullscreenStates) {
  EXPECT_EQ(to_string(NotificationState::running_d3d_full_screen), "running_d3d_full_screen");
  EXPECT_EQ(parse_notification_state("busy"), NotificationState::busy);
  EXPECT_EQ(parse_notification_state("Busy"), std::nullopt);
  EXPECT_TRUE(fullscreen_state(NotificationState::busy));
  EXPECT_TRUE(fullscreen_state(NotificationState::running_d3d_full_screen));
  EXPECT_FALSE(fullscreen_state(NotificationState::accepts_notifications));
  EXPECT_FALSE(fullscreen_state(NotificationState::presentation_mode));
  EXPECT_FALSE(fullscreen_state(NotificationState::app));
}

class Fullscreen : public ::testing::Test {
 protected:
  // Feeds `count` observations one second apart; returns the PID detected, if any.
  std::optional<std::uint32_t> run(int count, NotificationState state, std::uint32_t pid,
                                   std::optional<double> gpu) {
    std::optional<std::uint32_t> found;
    for (int second = 0; second < count; ++second) {
      if (const std::optional<std::uint32_t> now =
              detector_.observe({.state = state, .foreground_pid = pid, .gpu_3d_percent = gpu})) {
        found = now;
      }
      clock_.advance(seconds{1});
    }
    return found;
  }

  [[nodiscard]] FullscreenGpuDetector& detector() { return detector_; }
  [[nodiscard]] fakes::FakeClock& clock() { return clock_; }

 private:
  fakes::FakeClock clock_{WallTime{}};
  FullscreenGpuDetector detector_{FullscreenGpuPolicy{}, clock_};
};

TEST_F(Fullscreen, ExclusiveFullscreenWithGpuUseIsAGame) {
  EXPECT_EQ(run(10, NotificationState::running_d3d_full_screen, 4120, 16.7), std::nullopt);
  EXPECT_FALSE(detector().detected(4120));
  EXPECT_EQ(run(1, NotificationState::running_d3d_full_screen, 4120, 16.7), 4120U);
  EXPECT_TRUE(detector().detected(4120));
}

TEST_F(Fullscreen, BorderlessFullscreenWithGpuUseIsAGame) {
  EXPECT_EQ(run(11, NotificationState::busy, 4120, 14.7), 4120U);
  // A game stays one after it leaves the foreground, until it exits.
  run(5, NotificationState::accepts_notifications, 900, std::nullopt);
  EXPECT_TRUE(detector().detected(4120));
  detector().forget(4120);
  EXPECT_FALSE(detector().detected(4120));
}

TEST_F(Fullscreen, FullscreenWithoutGpuUseIsNotAGame) {
  EXPECT_EQ(run(60, NotificationState::busy, 4120, 0.2), std::nullopt);
  EXPECT_FALSE(detector().detected(4120));
  EXPECT_EQ(detector().candidate(), 4120U);  // still sampled while it holds the foreground
}

TEST_F(Fullscreen, NoCandidateOutsideFullscreen) {
  EXPECT_EQ(run(30, NotificationState::accepts_notifications, 4120, 50.0), std::nullopt);
  EXPECT_EQ(detector().candidate(), std::nullopt);
  EXPECT_EQ(run(30, NotificationState::busy, 0, 50.0), std::nullopt);
  EXPECT_EQ(detector().candidate(), std::nullopt);
}

TEST_F(Fullscreen, ForegroundChangeRestartsTheCandidate) {
  run(8, NotificationState::busy, 4120, 20.0);
  run(1, NotificationState::busy, 5000, 20.0);
  EXPECT_EQ(detector().candidate(), 5000U);
  EXPECT_EQ(run(8, NotificationState::busy, 4120, 20.0), std::nullopt);
  EXPECT_EQ(run(3, NotificationState::busy, 4120, 20.0), 4120U);
  EXPECT_FALSE(detector().detected(5000));
}

TEST_F(Fullscreen, LeavingFullscreenRestartsTheCandidate) {
  run(8, NotificationState::running_d3d_full_screen, 4120, 20.0);
  run(1, NotificationState::accepts_notifications, 4120, std::nullopt);
  EXPECT_EQ(run(10, NotificationState::running_d3d_full_screen, 4120, 20.0), std::nullopt);
  EXPECT_EQ(run(1, NotificationState::running_d3d_full_screen, 4120, 20.0), 4120U);
}

TEST_F(Fullscreen, AveragesOverTheLastWindow) {
  // Loading screens at 0 %, then rendering: the old samples leave the window.
  run(10, NotificationState::busy, 4120, 0.0);
  EXPECT_EQ(run(4, NotificationState::busy, 4120, 5.0), std::nullopt);  // 4 x 5 % in 11 samples
  EXPECT_EQ(run(1, NotificationState::busy, 4120, 5.0), 4120U);         // 5 x 5 % in 11 samples
}

TEST_F(Fullscreen, NeedsEnoughGpuSamples) {
  // GPU sampled only every 3 s: 4 samples in the window are not enough.
  for (int second = 0; second < 30; ++second) {
    const std::optional<double> gpu = second % 3 == 0 ? std::optional<double>{30.0} : std::nullopt;
    EXPECT_EQ(
        detector().observe(
            {.state = NotificationState::busy, .foreground_pid = 4120, .gpu_3d_percent = gpu}),
        std::nullopt);
    clock().advance(seconds{1});
  }
  // Once sampled every second, it is detected.
  EXPECT_EQ(run(2, NotificationState::busy, 4120, 30.0), 4120U);
}

TEST_F(Fullscreen, NoSamplingOnceDetected) {
  run(11, NotificationState::busy, 4120, 10.0);
  ASSERT_TRUE(detector().detected(4120));
  EXPECT_EQ(detector().candidate(), std::nullopt);
  EXPECT_EQ(run(1, NotificationState::busy, 4120, std::nullopt), std::nullopt);
}

TEST(FullscreenPolicy, ThresholdsAreConfigurable) {
  fakes::FakeClock clock{WallTime{}};
  FullscreenGpuDetector detector{
      FullscreenGpuPolicy{.min_gpu_percent = 50.0, .window = seconds{3}, .min_samples = 2}, clock};
  for (int second = 0; second < 4; ++second) {
    EXPECT_EQ(detector.observe(
                  {.state = NotificationState::busy, .foreground_pid = 7, .gpu_3d_percent = 40.0}),
              std::nullopt);
    clock.advance(seconds{1});
  }
  std::optional<std::uint32_t> found;
  for (int second = 0; second < 4 && !found; ++second) {
    found = detector.observe(
        {.state = NotificationState::busy, .foreground_pid = 7, .gpu_3d_percent = 60.0});
    clock.advance(seconds{1});
  }
  EXPECT_EQ(found, 7U);
}

}  // namespace
}  // namespace myclean::games
