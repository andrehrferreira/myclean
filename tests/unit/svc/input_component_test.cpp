#include "svc/input_component.hpp"

#include <chrono>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/input/incident.hpp"
#include "core/input/incident_record.hpp"
#include "core/ipc/input.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/model/result.hpp"
#include "core/store/database.hpp"
#include "fakes/fake_clock.hpp"
#include "phase1_fakes.hpp"

namespace myclean::svc {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

const WallTime noon = std::chrono::sys_days{std::chrono::October / 31 / 2026} + 12h;

// A paste whose clipboard owner did not answer for 700 ms: delayed rendering by Excel.
input::Incident delayed_paste(std::chrono::seconds after = 0s) {
  return input::Incident{.chord = input::Chord::paste,
                         .time = noon + after,
                         .latency = 20ms,
                         .owner_unanswered = 700ms,
                         .foreground = input::ProcessRef{.pid = 10, .image = "Code.exe"},
                         .owner = input::ProcessRef{.pid = 20, .image = "EXCEL.EXE"},
                         .formats = {{.id = 13, .name = "CF_UNICODETEXT"}},
                         .suspects = {"AnyDesk.exe"},
                         .cpu_percent = 12.5,
                         .memory_load_percent = 40};
}

// A copy that matches no fingerprint.
input::Incident slow_copy() {
  return input::Incident{.chord = input::Chord::copy, .time = noon - 1h, .latency = 400ms};
}

class InputComponentTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(harness_.start(input_).has_value()); }

  [[nodiscard]] Result<Json> record(const Json& incident) const {
    return harness_.call(ipc::input_record_incident_method, Json{{"incident", incident}});
  }
  [[nodiscard]] int code(const Json& incident) const {
    return harness_.code(ipc::input_record_incident_method, Json{{"incident", incident}});
  }
  [[nodiscard]] int missing_incident() const {
    return harness_.code(ipc::input_record_incident_method, Json::object());
  }
  [[nodiscard]] Json summary() const {
    Result<Json> reply = harness_.call(ipc::input_summary_method);
    EXPECT_TRUE(reply.has_value()) << reply.error().message;
    return reply.value_or(Json());
  }

 private:
  fakes::FakeClock clock_{noon};
  testing::ComponentHarness harness_{clock_};
  InputComponent input_;
};

TEST_F(InputComponentTest, StoresIncidentsAndSummarizesThemByMechanism) {
  EXPECT_EQ(summary(), (Json{{"groups", Json::array()}}));

  const Result<Json> first = record(input::to_record(delayed_paste()));
  ASSERT_TRUE(first.has_value()) << first.error().message;
  EXPECT_EQ(*first, (Json{{"id", 1}}));
  ASSERT_TRUE(record(input::to_record(slow_copy())).has_value());

  const Json groups = summary().at("groups");
  ASSERT_EQ(groups.size(), 2U);
  const Json& delayed = groups[0];
  EXPECT_EQ(delayed.at("mechanism"), "delayed_rendering");
  EXPECT_EQ(delayed.at("process"), "EXCEL.EXE");
  EXPECT_EQ(delayed.at("count"), 1);
  EXPECT_EQ(delayed.at("latest"), store::to_unix_seconds(noon));
  ASSERT_EQ(delayed.at("incidents").size(), 1U);
  const Json& incident = delayed.at("incidents")[0];
  EXPECT_EQ(incident.at("id"), 1);
  EXPECT_EQ(incident.at("owner_unanswered_ms"), 700);
  EXPECT_EQ(incident.at("cause"), (Json{{"pid", 20}, {"image", "EXCEL.EXE"}}));
  EXPECT_FALSE(incident.at("evidence").empty());
  EXPECT_EQ(incident.at("suspects"), Json::array({"AnyDesk.exe"}));

  EXPECT_EQ(groups[1].at("mechanism"), "unexplained");
  EXPECT_TRUE(groups[1].at("process").is_null());
  EXPECT_TRUE(groups[1].at("incidents")[0].at("cause").is_null());
}

TEST_F(InputComponentTest, ASummaryGroupHoldsTheNewestIncidentsAndCountsAll) {
  for (int index = 0; index < 25; ++index) {
    ASSERT_TRUE(record(input::to_record(delayed_paste(std::chrono::seconds{index}))).has_value());
  }
  const Json group = summary().at("groups").at(0);
  EXPECT_EQ(group.at("count"), 25);
  ASSERT_EQ(group.at("incidents").size(), ipc::max_summary_incidents);
  EXPECT_EQ(group.at("incidents")[0].at("time"), store::to_unix_seconds(noon + 24s));
}

TEST_F(InputComponentTest, RefusesRecordsOutsideTheSchemaWithoutStoringThem) {
  Json unknown = input::to_record(delayed_paste());
  unknown["command"] = "calc.exe";
  Json missing = input::to_record(delayed_paste());
  missing.erase("formats");
  Json control = input::to_record(delayed_paste());
  control["owner"]["image"] = "EXCEL.EXE\r\ncalc";
  Json long_text = input::to_record(delayed_paste());
  long_text["suspects"] = Json::array({std::string(261, 'a')});
  Json many = input::to_record(delayed_paste());
  many["suspects"] = Json(std::vector<std::string>(65, "a.exe"));
  Json deep = input::to_record(delayed_paste());
  deep["owner"]["image"] = Json{{"a", {{"b", {{"c", 1}}}}}};
  Json wrong = input::to_record(delayed_paste());
  wrong["latency_ms"] = -1;

  for (const Json& record :
       {unknown, missing, control, long_text, many, deep, wrong, Json("incident"), Json::array()}) {
    EXPECT_EQ(code(record), ipc::rpc_error::invalid_params) << record.dump();
  }
  EXPECT_EQ(missing_incident(), ipc::rpc_error::invalid_params);
  EXPECT_EQ(summary().at("groups"), Json::array());
}

}  // namespace
}  // namespace myclean::svc
