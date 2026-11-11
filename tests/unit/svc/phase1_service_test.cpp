#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/input/incident.hpp"
#include "core/input/incident_record.hpp"
#include "core/ipc/alerts.hpp"
#include "core/ipc/disk_health.hpp"
#include "core/ipc/input.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/status.hpp"
#include "core/ipc/telemetry.hpp"
#include "core/model/result.hpp"
#include "fakes/fake_clock.hpp"
#include "phase1_fakes.hpp"
#include "platform/ipc/pipe_client.hpp"
#include "svc/service.hpp"

// Every phase-1 component in one console-mode service over fakes, driven through its pipe as the
// CLI, the MCP server and the tray drive it.
namespace myclean::svc {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;
using namespace myclean::literals;

const WallTime noon = std::chrono::sys_days{std::chrono::October / 31 / 2026} + 12h;

class Phase1ServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fakes_.machine.volumes->list().front().free = 50_GiB;  // C: 5 % free: below the 10 % rule
    Result<std::unique_ptr<Service>> started =
        testing::start_phase1_service(fakes_, dir_.path(), pipe_, clock_);
    ASSERT_TRUE(started.has_value()) << started.error().message;
    service_ = std::move(*started);
    Result<platform::PipeClient> client = platform::PipeClient::connect(pipe_, 5s);
    ASSERT_TRUE(client.has_value()) << client.error().message;
    client_ = std::make_unique<platform::PipeClient>(std::move(*client));
  }

  [[nodiscard]] Json ok(std::string_view method, const Json& params = Json::object()) {
    Result<Json> result = client_->call(method, params);
    EXPECT_TRUE(result.has_value()) << method << ": " << result.error().message;
    return result.value_or(Json());
  }
  [[nodiscard]] Result<Json> call(std::string_view method, const Json& params) {
    return client_->call(method, params);
  }
  [[nodiscard]] int code(std::string_view method, const Json& params) {
    const Result<std::string> response = client_->exchange(ipc::make_request(3, method, params));
    return testing::rpc_code(response);
  }

  [[nodiscard]] platform::PipeClient& client() { return *client_; }
  [[nodiscard]] Service& service() { return *service_; }

 private:
  testing::TempDir dir_;
  fakes::FakeClock clock_{noon};
  std::wstring pipe_ = testing::unique_pipe_name();
  testing::Phase1Fakes fakes_;
  std::unique_ptr<Service> service_;
  std::unique_ptr<platform::PipeClient> client_;
};

TEST_F(Phase1ServiceTest, RunsEveryComponentInOrderAndReportsTheirHealth) {
  const Result<ipc::StatusReply> status = client().status();
  ASSERT_TRUE(status.has_value()) << status.error().message;
  EXPECT_EQ(status->components, (std::vector<std::string>{"alerts", "trust", "telemetry",
                                                          "disk_health", "events", "input"}));
  ASSERT_EQ(status->component_health.size(), 6U);
  EXPECT_TRUE(status->healthy);
  const ipc::ComponentHealth& events = status->component_health[4];
  EXPECT_TRUE(events.healthy);
  EXPECT_EQ(events.unavailable, std::vector<std::string>{"ETW unavailable: not elevated"});

  const auto stopping = std::chrono::steady_clock::now();
  EXPECT_TRUE(service().stop().has_value());
  EXPECT_LT(std::chrono::steady_clock::now() - stopping, 5s);
}

TEST_F(Phase1ServiceTest, ProcessTreesCarryTheTrustSignal) {
  const Json top = ok(ipc::processes_top_method, Json{{"sort", "memory"}, {"limit", 3}});
  const Json& trees = top.at("trees");
  ASSERT_EQ(trees.size(), 3U);
  EXPECT_EQ(trees[0].at("pid"), 200);
  EXPECT_EQ(trees[0].at("trust").at("unrecognized"), true);
  EXPECT_EQ(trees[0].at("trust").at("reasons"),
            (Json{"not-in-knowledge-base", "unsigned", "user-writable-path"}));
  EXPECT_EQ(trees[1].at("pid"), 100);
  EXPECT_EQ(trees[1].at("trust").at("unrecognized"), false);
  EXPECT_EQ(trees[1].at("trust").at("signer"), "Microsoft Corporation");
  EXPECT_FALSE(trees[1].at("trust").at("description").is_null());
  EXPECT_TRUE(trees[2].at("trust").is_null());  // pid 300 cannot be opened
}

TEST_F(Phase1ServiceTest, ExplainsAProcessOrSaysItWasNotFound) {
  const Json child = ok(ipc::processes_explain_method, Json{{"pid", 101}});
  EXPECT_EQ(child.at("pid"), 101);
  EXPECT_EQ(child.at("parent_pid"), 100);
  EXPECT_EQ(child.at("path"), R"(C:\Program Files\PowerToys\PowerToys.exe)");
  EXPECT_EQ(child.at("tree").at("pid"), 100);
  EXPECT_EQ(child.at("tree").at("processes"), 2);
  EXPECT_EQ(child.at("members").size(), 2U);
  EXPECT_EQ(child.at("trust").at("status"), "signed");
  EXPECT_EQ(child.at("kb").at("category"), "utility");
  EXPECT_EQ(child.at("kb").at("needed_at_startup"), false);
  EXPECT_TRUE(child.at("memory_percent").is_number());

  const Json unreadable = ok(ipc::processes_explain_method, Json{{"pid", 300}});
  EXPECT_TRUE(unreadable.at("trust").is_null());
  EXPECT_TRUE(unreadable.at("kb").is_null());
  EXPECT_TRUE(unreadable.at("path").is_null());

  const Result<Json> gone = call(ipc::processes_explain_method, Json{{"pid", 99999}});
  ASSERT_FALSE(gone.has_value());
  EXPECT_EQ(gone.error().code, Errc::not_found);
  EXPECT_NE(gone.error().message.find("process 99999 was not found"), std::string::npos);
  EXPECT_EQ(code(ipc::processes_explain_method, Json{{"pid", "C:\\x.exe"}}),
            ipc::rpc_error::invalid_params);
}

TEST_F(Phase1ServiceTest, LatestListsEveryLogicalProcessor) {
  const Json processors = ok(ipc::metrics_latest_method).at("cpu").at("processors");
  ASSERT_EQ(processors.size(), 1U);  // the fake machine reports processor 0,0 only
  EXPECT_EQ(processors[0].at("entity"), "0_0");
  EXPECT_EQ(processors[0].at("utility"), 30.0);
}

TEST_F(Phase1ServiceTest, VolumeFactsRaiseAlerts) {
  const Json alerts = ok(ipc::alerts_list_method).at("alerts");
  ASSERT_EQ(alerts.size(), 1U);
  EXPECT_EQ(alerts[0].at("rule"), "volume.free-space-low");
  EXPECT_EQ(alerts[0].at("entity"), "C");
  EXPECT_EQ(ok(ipc::alerts_badge_method), (Json{{"count", 1}, {"worst", "warning"}}));
  // Only the owner's tray may dismiss it: this client is not the configured tray.
  const Result<Json> dismissed =
      call(ipc::alerts_dismiss_method, Json{{"rule", "volume.free-space-low"}, {"entity", "C"}});
  ASSERT_FALSE(dismissed.has_value());
  EXPECT_EQ(dismissed.error().code, Errc::refused);
  EXPECT_EQ(ok(ipc::alerts_badge_method).at("count"), 1);
}

TEST_F(Phase1ServiceTest, DiskHealthMapsDrivesToVolumes) {
  Json drives;
  ASSERT_TRUE(testing::eventually([&] {
    drives = ok(ipc::disks_health_method).at("drives");
    return drives.size() == 2;
  }));
  EXPECT_EQ(drives[0].at("volumes"), Json::array({"C:"}));
  EXPECT_EQ(drives[1].at("readable"), false);
}

TEST_F(Phase1ServiceTest, RecordsIncidentsFromTheTray) {
  const input::Incident incident{.chord = input::Chord::copy, .time = noon, .latency = 400ms};
  EXPECT_EQ(ok(ipc::input_record_incident_method, Json{{"incident", input::to_record(incident)}}),
            (Json{{"id", 1}}));
  EXPECT_EQ(ok(ipc::input_summary_method).at("groups").size(), 1U);

  Json smuggled = input::to_record(incident);
  smuggled["path"] = R"(C:\Windows\System32\cmd.exe)";
  EXPECT_EQ(code(ipc::input_record_incident_method, Json{{"incident", smuggled}}),
            ipc::rpc_error::invalid_params);
  EXPECT_EQ(ok(ipc::input_summary_method).at("groups").at(0).at("count"), 1);
}

}  // namespace
}  // namespace myclean::svc
