#include "tray/pipe_link.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include "../tray_model/tray_model_test_support.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/model/result.hpp"
#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/poller.hpp"
#include "platform/ipc/pipe_server.hpp"
#include "platform/ipc/security.hpp"

namespace myclean::tray {
namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;

constexpr std::int64_t sample_time = 1'000'000;

// Never the real \\.\pipe\myclean.
std::wstring unique_pipe_name() {
  static int counter = 0;
  return LR"(\\.\pipe\myclean-tray-test-)" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
         std::to_wstring(++counter);
}

// An in-process service that answers metrics.latest the way the real one does.
class FakeService {
 public:
  explicit FakeService(std::wstring name) : name_(std::move(name)) {
    EXPECT_TRUE(methods_
                    .add("metrics.latest", {},
                         [](const Json&) -> Result<Json> {
                           return tray_model::testing::latest_json(sample_time);
                         })
                    .has_value());
  }

  void start() {
    const Result<std::string> owner = platform::current_user_sid();
    ASSERT_TRUE(owner.has_value());
    Result<std::unique_ptr<platform::PipeServer>> server =
        platform::PipeServer::start(platform::PipeServerOptions{
            .name = name_,
            .owner_sid = *owner,
            .handler = [this](std::string_view request) { return methods_.handle_frame(request); },
            .on_error = {}});
    ASSERT_TRUE(server.has_value()) << server.error().message;
    server_ = std::move(*server);
  }

  void stop() { server_.reset(); }

 private:
  std::wstring name_;
  ipc::Dispatcher methods_;
  std::unique_ptr<platform::PipeServer> server_;
};

TEST(PipeLinkTest, NoServiceMeansUnreachable) {
  PipeLink link(unique_pipe_name(), 2s);
  const Result<Json> reply = link.call("metrics.latest", Json::object());
  ASSERT_FALSE(reply.has_value());
  EXPECT_EQ(reply.error().code, Errc::not_found);
  EXPECT_FALSE(link.reachable());
}

TEST(PipeLinkTest, CallsTheServiceAndReportsItsErrorsAsAnswers) {
  const std::wstring name = unique_pipe_name();
  FakeService service(name);
  service.start();
  PipeLink link(name, 2s);
  const Result<Json> reply = link.call("metrics.latest", Json::object());
  ASSERT_TRUE(reply.has_value()) << reply.error().message;
  EXPECT_EQ(reply->at("time"), sample_time);
  EXPECT_TRUE(link.reachable());
  const Result<Json> unknown = link.call("alerts.list", Json::object());
  ASSERT_FALSE(unknown.has_value());
  EXPECT_NE(unknown.error().message.find("-32601"), std::string::npos);
  EXPECT_TRUE(link.reachable()) << "an error answer still comes from the service";
}

TEST(PipeLinkTest, FollowsTheServiceThroughARestart) {
  const std::wstring name = unique_pipe_name();
  FakeService service(name);
  service.start();
  PipeLink link(name, 2s);
  ASSERT_TRUE(link.call("metrics.latest", Json::object()).has_value());
  service.stop();
  EXPECT_FALSE(link.call("metrics.latest", Json::object()).has_value());
  EXPECT_FALSE(link.reachable());
  service.start();
  EXPECT_TRUE(link.call("metrics.latest", Json::object()).has_value());
  EXPECT_TRUE(link.reachable());
}

TEST(PipeLinkTest, PollerAndModelOverThePipe) {
  const std::wstring name = unique_pipe_name();
  FakeService service(name);
  service.start();
  PipeLink link(name, 2s);
  tray_model::Poller poller(link, tray_model::live_window);
  tray_model::DashboardModel model;
  model.apply(poller.poll(tray_model::from_unix_seconds(sample_time), false));
  EXPECT_TRUE(model.service_reachable());
  EXPECT_EQ(model.tray_status().state, tray_model::TrayState::normal);
  service.stop();
  model.apply(poller.poll(tray_model::from_unix_seconds(sample_time + 1), false));
  EXPECT_FALSE(model.service_reachable());
  EXPECT_EQ(model.tray_status().state, tray_model::TrayState::service_down);
  EXPECT_EQ(model.metrics().value("cpu.utility", "system"), 42.0) << "the last values stay";
}

}  // namespace
}  // namespace myclean::tray
