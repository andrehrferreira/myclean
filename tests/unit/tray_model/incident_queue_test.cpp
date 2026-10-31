#include "core/tray_model/incident_queue.hpp"

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/input/incident.hpp"
#include "core/input/incident_record.hpp"
#include "core/model/result.hpp"
#include "core/tray_model/replies.hpp"
#include "tray_model_test_support.hpp"

namespace myclean::tray_model {
namespace {

using namespace std::chrono_literals;
using testing::FakeLink;
using testing::Json;

// An incident told apart by its latency.
input::Incident incident(int latency_ms) {
  return input::Incident{.chord = input::Chord::copy,
                         .time = from_unix_seconds(1'000'000),
                         .latency = std::chrono::milliseconds{latency_ms}};
}

// A service that stores every incident it is sent and answers with its row id.
class IncidentQueueTest : public ::testing::Test {
 protected:
  void SetUp() override {
    link_.on("input.record_incident", [this](const Json& params) -> Result<Json> {
      const Result<input::Incident> parsed = input::parse_record(params.at("incident"));
      if (!parsed) {
        return fail(Errc::invalid_argument, "json-rpc error -32602: " + parsed.error().message);
      }
      stored_.push_back(parsed->latency.count());
      return Json{{"id", static_cast<std::int64_t>(stored_.size())}};
    });
  }

  [[nodiscard]] FakeLink& link() { return link_; }
  [[nodiscard]] const std::vector<std::int64_t>& stored() const { return stored_; }

 private:
  FakeLink link_;
  std::vector<std::int64_t> stored_;  // latencies, in arrival order
};

TEST_F(IncidentQueueTest, ForwardsInOrderInTheRecordForm) {
  IncidentQueue queue;
  queue.record(incident(300));
  queue.record(incident(400));
  EXPECT_EQ(queue.counts().queued, 2U);
  EXPECT_EQ(queue.forward(link()), 2);
  EXPECT_EQ(stored(), (std::vector<std::int64_t>{300, 400}));
  EXPECT_EQ(queue.counts(),
            (IncidentQueue::Counts{.sent = 2, .dropped = 0, .rejected = 0, .queued = 0}));
  EXPECT_EQ(queue.forward(link()), 0);
  EXPECT_EQ(link().calls().size(), 2U);
}

TEST_F(IncidentQueueTest, KeepsIncidentsWhileTheServiceIsDown) {
  IncidentQueue queue;
  queue.record(incident(300));
  link().set_online(false);
  EXPECT_EQ(queue.forward(link()), 0);
  EXPECT_EQ(link().calls().size(), 1U) << "stops at the first call that does not get through";
  queue.record(incident(400));
  EXPECT_EQ(queue.counts().queued, 2U);
  link().set_online(true);
  EXPECT_EQ(queue.forward(link()), 2);
  EXPECT_EQ(stored(), (std::vector<std::int64_t>{300, 400}));
}

TEST_F(IncidentQueueTest, DropsTheOldestBeyondItsCapacityAndCountsThem) {
  IncidentQueue queue(3);
  link().set_online(false);
  for (int latency = 300; latency < 800; latency += 100) {
    queue.record(incident(latency));
    (void)queue.forward(link());
  }
  EXPECT_EQ(queue.counts(),
            (IncidentQueue::Counts{.sent = 0, .dropped = 2, .rejected = 0, .queued = 3}));
  link().set_online(true);
  EXPECT_EQ(queue.forward(link()), 3);
  EXPECT_EQ(stored(), (std::vector<std::int64_t>{500, 600, 700}));
}

TEST_F(IncidentQueueTest, AnIncidentTheServiceRefusesIsNotResent) {
  // input::to_record() always fits the schema, so here the service itself refuses one incident.
  std::vector<std::int64_t> accepted;
  link().on("input.record_incident", [&accepted](const Json& params) -> Result<Json> {
    const auto latency = params.at("incident").at("latency_ms").get<std::int64_t>();
    if (latency == 300) {
      return fail(Errc::invalid_argument, "json-rpc error -32602: incident: refused");
    }
    accepted.push_back(latency);
    return Json{{"id", static_cast<std::int64_t>(accepted.size())}};
  });
  IncidentQueue queue;
  queue.record(incident(300));
  queue.record(incident(400));
  EXPECT_EQ(queue.forward(link()), 1);
  EXPECT_EQ(accepted, std::vector<std::int64_t>{400});
  EXPECT_EQ(queue.counts(),
            (IncidentQueue::Counts{.sent = 1, .dropped = 0, .rejected = 1, .queued = 0}));
}

TEST_F(IncidentQueueTest, RecordingNeverWaitsForAForwardInProgress) {
  IncidentQueue queue;
  queue.record(incident(300));
  std::vector<int> recorded_during_call;
  // The service answers slowly; the watchdog records meanwhile, from its own thread.
  link().on("input.record_incident", [&](const Json&) -> Result<Json> {
    std::thread watchdog([&queue] { queue.record(incident(500)); });
    watchdog.join();
    recorded_during_call.push_back(1);
    return Json{{"id", 1}};
  });
  EXPECT_EQ(queue.forward(link()), 1);
  EXPECT_EQ(recorded_during_call.size(), 1U);
  EXPECT_EQ(queue.counts().queued, 1U) << "the incident recorded meanwhile waits for the next call";
}

}  // namespace
}  // namespace myclean::tray_model
