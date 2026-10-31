#include "tray/poll_thread.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "../tray_model/tray_model_test_support.hpp"
#include "core/input/incident.hpp"
#include "core/model/result.hpp"
#include "core/tray_model/incident_queue.hpp"
#include "core/tray_model/poller.hpp"
#include "core/tray_model/replies.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::tray {
namespace {

using namespace std::chrono_literals;
using tray_model::testing::FakeLink;
using tray_model::testing::Json;

constexpr std::int64_t start = 1'000'000;

// Counts the updates the poll thread reports and lets the test wait for the next one.
class Updates {
 public:
  void notify() {
    const std::scoped_lock lock(mutex_);
    ++count_;
    changed_.notify_all();
  }

  // Waits (bounded) until at least `count` updates happened.
  bool wait_for(int count) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 10s, [&] { return count_ >= count; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  int count_ = 0;
};

class PollThreadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    link_.on("metrics.latest",
             [](const Json&) -> Result<Json> { return tray_model::testing::latest_json(start); });
    link_.on("processes.top",
             [](const Json&) -> Result<Json> { return tray_model::testing::top_json(); });
  }

  [[nodiscard]] FakeLink& link() { return link_; }
  [[nodiscard]] const fakes::FakeClock& clock() const { return clock_; }
  [[nodiscard]] SharedModel& shared() { return shared_; }
  [[nodiscard]] Updates& updates() { return updates_; }

 private:
  FakeLink link_;
  fakes::FakeClock clock_{tray_model::from_unix_seconds(start)};
  SharedModel shared_;
  Updates updates_;
};

TEST_F(PollThreadTest, PollsAtOnceAndAppliesTheResult) {
  {
    const PollThread thread(link(), clock(), shared(), [this] { updates().notify(); });
    ASSERT_TRUE(updates().wait_for(1));
    const std::scoped_lock lock(shared().mutex);
    EXPECT_TRUE(shared().model.service_reachable());
    EXPECT_EQ(shared().model.metrics_time(), tray_model::from_unix_seconds(start));
    EXPECT_FALSE(shared().model.top_by_cpu()) << "process trees only while the dashboard is open";
  }
  EXPECT_FALSE(link().calls().empty());
}

TEST_F(PollThreadTest, OpeningTheDashboardPollsTheProcessTreesAtOnce) {
  PollThread thread(link(), clock(), shared(), [this] { updates().notify(); });
  ASSERT_TRUE(updates().wait_for(1));
  thread.set_dashboard_open(true);
  // The next poll runs at once (without waiting for the second) and carries the trees.
  ASSERT_TRUE(updates().wait_for(2));
  const std::scoped_lock lock(shared().mutex);
  const auto& trees = shared().model.top_by_cpu();
  ASSERT_TRUE(trees.has_value());
  EXPECT_EQ((*trees).size(), 2U);
}

TEST_F(PollThreadTest, ARequestIsSentAtOnceAndItsAnswerApplied) {
  link().on("processes.explain",
            [](const Json&) -> Result<Json> { return tray_model::testing::explain_json(); });
  PollThread thread(link(), clock(), shared(), [this] { updates().notify(); });
  ASSERT_TRUE(updates().wait_for(1));
  thread.request(tray_model::ExplainProcess{.pid = 101});
  ASSERT_TRUE(updates().wait_for(2));
  const std::scoped_lock lock(shared().mutex);
  const auto& explanation = shared().model.explanation();
  ASSERT_TRUE(explanation.has_value());
  ASSERT_TRUE((*explanation).has_value());
  EXPECT_EQ((*explanation).value().pid, 101U);
  EXPECT_EQ(link().calls_of("processes.explain").size(), 1U);
}

TEST_F(PollThreadTest, ForwardsTheWatchdogsIncidents) {
  int recorded = 0;
  link().on("input.record_incident", [&recorded](const Json&) -> Result<Json> {
    ++recorded;
    return Json{{"id", recorded}};
  });
  tray_model::IncidentQueue incidents;
  incidents.record(input::Incident{
      .chord = input::Chord::copy, .time = tray_model::from_unix_seconds(start), .latency = 400ms});
  {
    const PollThread thread(link(), clock(), shared(), [this] { updates().notify(); }, &incidents);
    ASSERT_TRUE(updates().wait_for(1));
  }
  EXPECT_EQ(recorded, 1);
  EXPECT_EQ(incidents.counts().sent, 1U);
  const std::scoped_lock lock(shared().mutex);
  const auto& counts = shared().model.incident_counts();
  ASSERT_TRUE(counts.has_value());
  EXPECT_EQ((*counts).sent, 1U);
}

TEST_F(PollThreadTest, StopsPromptlyWhileTheServiceIsDown) {
  link().set_online(false);
  const auto begin = std::chrono::steady_clock::now();
  {
    const PollThread thread(link(), clock(), shared(), [this] { updates().notify(); });
    ASSERT_TRUE(updates().wait_for(1));
    const std::scoped_lock lock(shared().mutex);
    EXPECT_FALSE(shared().model.service_reachable());
  }
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 5s);
}

}  // namespace
}  // namespace myclean::tray
