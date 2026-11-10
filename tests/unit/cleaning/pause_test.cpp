// Pauses of freeze suspects as plans (input-watchdog spec, "Pause Requires Tray Approval", "One
// Suspect at a Time"): proposed by the owner or an MCP client, run only once the owner approves
// them, one at a time, and verified when the pause is over. FakeJobs stands in for the process
// control: nothing real is suspended.
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "cleaning_fixture.hpp"
#include "core/cleaning/service.hpp"
#include "core/elimination/keeper.hpp"
#include "core/input/incident.hpp"
#include "core/input/incident_store.hpp"
#include "core/model/units.hpp"
#include "core/os/process_control.hpp"
#include "core/telemetry/process_collector.hpp"
#include "fakes/fake_jobs.hpp"

namespace myclean::cleaning {
namespace {

using namespace std::chrono_literals;
using testing::Json;

class PausePlanTest : public testing::CleaningFixture {
 protected:
  PausePlanTest() {
    anydesk_ = jobs_.add_process(20, testing::noon - 8h);
    view_ = std::make_shared<telemetry::ProcessView>(telemetry::ProcessView{
        .time = testing::noon,
        .rows = {telemetry::ProcessRow{.info = {.pid = anydesk_.pid,
                                                .created = anydesk_.created,
                                                .image = "AnyDesk.exe",
                                                .session = 1}},
                 telemetry::ProcessRow{.info = {.pid = 40,
                                                .created = testing::noon - 9h,
                                                .image = "PowerToys.KeyboardManagerEngine.exe",
                                                .session = 1}}}});
    keeper_ = std::make_unique<elimination::PauseKeeper>(
        store(), clock(),
        elimination::KeeperSources{
            .processes = &jobs_,
            .view = [this] { return std::shared_ptr<const telemetry::ProcessView>(view_); },
            .protects = [](const os::ProcessKey& /*key*/) { return false; }});
    set_pauses(keeper_.get());
  }

  Json get(std::int64_t plan) { return call("plans.get", Json{{"id", plan}}); }

  // Freezes recorded for 8 days, 2 a day.
  void record_freezes() {
    for (int day = 8; day >= 1; --day) {
      for (const auto hour : {2h, 5h}) {
        ASSERT_TRUE(
            input::save_incident(
                store(), input::Incident{.time = testing::noon - std::chrono::days{day} + hour,
                                         .latency = 400ms})
                .has_value());
      }
    }
  }

  fakes::FakeJobs& jobs() { return jobs_; }
  [[nodiscard]] const os::ProcessKey& anydesk() const { return anydesk_; }
  elimination::PauseKeeper& keeper() { return *keeper_; }

 private:
  fakes::FakeJobs jobs_;
  os::ProcessKey anydesk_;
  std::shared_ptr<telemetry::ProcessView> view_;
  std::unique_ptr<elimination::PauseKeeper> keeper_;
};

// Specs "Pause waits for approval", "MCP cannot start a pause", "Second pause refused".
TEST_F(PausePlanTest, APauseRunsOnlyOnceApprovedAndOneAtATime) {
  start();
  work();
  EXPECT_EQ(error_of("plans.propose_pause", Json{{"suspect", 2}}, true).at("data").at("errc"),
            "invalid_state");  // no baseline yet
  record_freezes();
  EXPECT_EQ(error_of("plans.propose_pause", Json{{"suspect", 9}}, true).at("data").at("errc"),
            "not_found");
  EXPECT_EQ(error_of("plans.propose_pause", Json{{"suspect", 2}, {"hours", 500}}, true)
                .at("data")
                .at("errc"),
            "invalid_argument");

  const Json proposed =
      call("plans.propose_pause", Json{{"suspect", 2}, {"client", "claude"}}, false);
  const std::int64_t id = proposed.at("plan").get<std::int64_t>();
  EXPECT_EQ(proposed.at("state"), "awaiting_approval");
  Json plan = get(id);
  EXPECT_EQ(plan.at("kind"), "pause");
  EXPECT_EQ(plan.at("proposer"), "mcp:claude");
  EXPECT_EQ(plan.at("params").at("mode"), "suspend");
  EXPECT_EQ(error_of("plans.propose_pause", Json{{"suspect", 2}}, true).at("data").at("errc"),
            "already_exists");
  work();
  EXPECT_EQ(jobs().process(anydesk()).suspend_count, 0U);  // waits for the owner

  approve_all(id);
  work();
  EXPECT_EQ(jobs().process(anydesk()).suspend_count, 1U);
  plan = get(id);
  EXPECT_EQ(plan.at("state"), "applied");
  EXPECT_EQ(plan.at("verification").at("result"), "pending");

  // The keyboard-hook owner is proposed as a guided pause; approved, it waits for the first.
  const std::int64_t guided =
      call("plans.propose_pause", Json{{"suspect", 1}}).at("plan").get<std::int64_t>();
  EXPECT_EQ(get(guided).at("params").at("mode"), "guided");
  approve_all(guided);
  work();
  EXPECT_EQ(get(guided).at("state"), "approved");

  clock().advance(24h);
  keeper().tick();  // the first pause is over: AnyDesk runs again
  EXPECT_EQ(jobs().process(anydesk()).suspend_count, 0U);
  work();
  plan = get(id);
  EXPECT_EQ(plan.at("state"), "verified");
  EXPECT_EQ(plan.at("verification").at("result"), "better");
  EXPECT_EQ(get(guided).at("state"), "applied");  // the guided pause started
  EXPECT_EQ(jobs().process(anydesk()).suspend_count, 0U);
}

}  // namespace
}  // namespace myclean::cleaning
