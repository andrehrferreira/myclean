// The governor's IPC methods (core/ipc/governor.hpp): shapes, parameters, and the tray-only
// methods refused for every other caller.

#include "core/governing/methods.hpp"

#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/governing/keys.hpp"
#include "core/governor/forest.hpp"
#include "core/ipc/governor.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/metrics.hpp"
#include "governing_test_support.hpp"

namespace myclean::governing {
namespace {

using namespace myclean::literals;
using Json = nlohmann::json;
using testing::Machine;

class GovernorMethods : public ::testing::Test {
 protected:
  void SetUp() override {
    machine_.start();
    ASSERT_TRUE(register_methods(methods_, machine_.governor(), machine_.store()));
  }

  Result<Json> call(std::string_view method, const Json& params, bool tray) {
    return ipc::parse_response(
        methods_.handle(ipc::make_request(3, method, params), ipc::Caller{.tray = tray}), 3);
  }

  [[nodiscard]] Machine& machine() { return machine_; }

 private:
  Machine machine_;
  ipc::Dispatcher methods_;
};

TEST_F(GovernorMethods, TrayOnlyMethodsAreRefusedForOtherCallers) {
  const governor::ProcessKey root = machine().spawn(300, R"(C:\Tools\bigjob.exe)");
  const Json choose{
      {"pid", root.pid}, {"created", to_nanoseconds(root.created)}, {"choice", "allow_once"}};
  const Json user_state{{"state", "busy"}, {"foreground_pid", 1234}};
  for (const auto& [method, params] :
       {std::pair{ipc::governor_choose_method, choose},
        std::pair{ipc::governor_pause_method, Json{{"minutes", 10}}},
        std::pair{ipc::governor_game_mode_method, Json{{"override", "on"}}},
        std::pair{ipc::governor_user_state_method, user_state}}) {
    const Result<Json> refused = call(method, params, false);
    ASSERT_FALSE(refused.has_value()) << method;
    EXPECT_EQ(refused.error().code, Errc::refused) << method;
    EXPECT_TRUE(call(method, params, true).has_value()) << method;
  }
  EXPECT_EQ(machine().governor().status().at("game_mode").at("override"), "on");
}

TEST_F(GovernorMethods, StatusAndEventsAreReadableByAnyClient) {
  machine().spawn(100, R"(C:\Users\owner\.cargo\bin\cargo.exe)");
  machine().second();
  const Result<Json> status = call(ipc::governor_status_method, Json::object(), false);
  ASSERT_TRUE(status.has_value()) << status.error().message;
  EXPECT_EQ(status->at("mode"), "act");
  EXPECT_EQ(status->at("pressure").at("state"), "normal");
  EXPECT_EQ(status->at("pressure").at("metrics").at("commit_limit"), (150_GiB).count());
  EXPECT_EQ(status->at("jobs").size(), 2U);
  EXPECT_TRUE(status->at("paused_until").is_null());
  EXPECT_EQ(status->at("events").at(0).at("kind"), "place_job");

  const Result<Json> events = call(ipc::governor_events_method, Json{{"limit", 1}}, false);
  ASSERT_TRUE(events.has_value()) << events.error().message;
  ASSERT_EQ(events->at("events").size(), 1U);
  const Json& event = events->at("events").at(0);
  EXPECT_EQ(event.at("kind"), "place_job");
  EXPECT_EQ(event.at("root").at("pid"), 100);
  EXPECT_EQ(event.at("class"), "build");
  EXPECT_EQ(event.at("actor"), "governor");
  EXPECT_TRUE(event.at("step").is_null());
}

TEST_F(GovernorMethods, InvalidValuesAreRefused) {
  EXPECT_EQ(call(ipc::governor_pause_method, Json{{"minutes", 1441}}, true).error().code,
            Errc::invalid_argument);
  EXPECT_EQ(call(ipc::governor_game_mode_method, Json{{"override", "maybe"}}, true).error().code,
            Errc::invalid_argument);
  EXPECT_EQ(
      call(ipc::governor_user_state_method, Json{{"state", "dark"}, {"foreground_pid", 1}}, true)
          .error()
          .code,
      Errc::invalid_argument);
  EXPECT_EQ(call(ipc::governor_choose_method,
                 Json{{"pid", 1}, {"created", 0}, {"choice", "ignore"}}, true)
                .error()
                .code,
            Errc::invalid_argument);
  EXPECT_EQ(call(ipc::governor_events_method, Json{{"limit", 0}}, false).error().code,
            Errc::invalid_argument);
  // A path is not a token: refused before anything runs.
  EXPECT_EQ(call(ipc::governor_game_mode_method, Json{{"override", R"(C:\x)"}}, true).error().code,
            Errc::invalid_argument);
}

TEST_F(GovernorMethods, PauseReportsWhenItEnds) {
  const Result<Json> paused = call(ipc::governor_pause_method, Json{{"minutes", 30}}, true);
  ASSERT_TRUE(paused.has_value());
  EXPECT_EQ(paused->at("paused_until"),
            store::to_unix_seconds(machine().clock().now()) + std::int64_t{1800});
  const Result<Json> resumed = call(ipc::governor_pause_method, Json{{"minutes", 0}}, true);
  ASSERT_TRUE(resumed.has_value());
  EXPECT_TRUE(resumed->at("paused_until").is_null());
}

}  // namespace
}  // namespace myclean::governing
