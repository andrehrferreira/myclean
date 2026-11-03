// The tray's reading of governor.status, the parameters of the owner's governor requests and the
// toasts of governor notifications (governor spec, "Rate-Limited Owner Notifications").

#include "core/tray_model/governor.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/tray_model/replies.hpp"
#include "tray_model_test_support.hpp"

namespace myclean::tray_model {
namespace {

using testing::governor_notification_json;
using testing::governor_status_json;
using testing::Json;

GovernorStatus status_with(Json notifications) {
  Result<GovernorStatus> status =
      parse_governor_status(governor_status_json(std::move(notifications)));
  EXPECT_TRUE(status.has_value()) << status.error().message;
  return status.value_or(GovernorStatus{});
}

TEST(GovernorStatusTest, ParsesTheServiceShape) {
  const Result<GovernorStatus> status = parse_governor_status(governor_status_json(
      Json::array({governor_notification_json(4, 1000, 300, "bigjob.exe", "suspend")})));
  ASSERT_TRUE(status.has_value()) << status.error().message;
  EXPECT_EQ(status->mode, "act");
  EXPECT_FALSE(status->paused_until);
  EXPECT_TRUE(status->pressure);
  EXPECT_FALSE(status->clearing);
  EXPECT_EQ(status->reasons, std::vector<std::string>{"cpu"});
  EXPECT_TRUE(status->game_mode);
  EXPECT_EQ(status->game_override, "auto");
  EXPECT_EQ(status->build_cpu_percent, 40);
  ASSERT_EQ(status->games.size(), 1U);
  EXPECT_EQ(status->games[0].image, "title.exe");
  EXPECT_EQ(status->games[0].reason, "library_root");
  ASSERT_EQ(status->trees.size(), 1U);
  EXPECT_EQ(status->trees[0].root, (GovernorProcess{.pid = 300, .created = 300000}));
  EXPECT_EQ(status->trees[0].step, "ecoqos");
  ASSERT_EQ(status->notifications.size(), 1U);
  const GovernorNotification& notification = status->notifications[0];
  EXPECT_EQ(notification.id, 4);
  EXPECT_EQ(notification.time, from_unix_seconds(1000));
  EXPECT_EQ(notification.root, (GovernorProcess{.pid = 300, .created = 300000}));
  EXPECT_EQ(notification.image, "bigjob.exe");
  EXPECT_EQ(notification.process_class, "default");
  EXPECT_EQ(notification.step, "suspend");
  EXPECT_EQ(notification.reason, "cpu");
  EXPECT_EQ(notification.members, 3);
  ASSERT_EQ(status->events.size(), 2U);
  EXPECT_EQ(status->events[0].kind, "apply_step");
  EXPECT_EQ(status->events[0].step, "ecoqos");
  EXPECT_FALSE(status->events[1].step);
  EXPECT_EQ(status->events[1].detail, "game mode on (override auto)");
}

TEST(GovernorStatusTest, ReadsAPauseAndRejectsAMalformedReply) {
  Json paused = governor_status_json();
  paused["paused_until"] = 4600;
  const Result<GovernorStatus> status = parse_governor_status(paused);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status->paused_until, from_unix_seconds(4600));

  Json broken = governor_status_json();
  broken.erase("game_mode");
  const Result<GovernorStatus> refused = parse_governor_status(broken);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code, Errc::invalid_data);
}

TEST(GovernorStatusTest, ParamsMatchTheMethods) {
  EXPECT_EQ(governor_choose_params(GovernorProcess{.pid = 300, .created = 300000}, "allow_once"),
            (Json{{"pid", 300}, {"created", 300000}, {"choice", "allow_once"}}));
  EXPECT_EQ(governor_pause_params(std::chrono::minutes{60}), (Json{{"minutes", 60}}));
  EXPECT_EQ(governor_game_mode_params("off"), (Json{{"override", "off"}}));
  EXPECT_EQ(governor_user_state_params(
                UserStateReading{.state = "running_d3d_full_screen", .foreground_pid = 900}),
            (Json{{"state", "running_d3d_full_screen"}, {"foreground_pid", 900}}));
}

TEST(GovernorToastTest, SaysWhatWasDoneAndWhereToAnswer) {
  const GovernorToast suspended = governor_toast(
      status_with(Json::array({governor_notification_json(1, 1000, 300, "bigjob.exe", "suspend")}))
          .notifications[0]);
  EXPECT_EQ(suspended.title, "MyClean suspended bigjob.exe");
  EXPECT_NE(suspended.detail.find("3 processes"), std::string::npos);
  EXPECT_NE(suspended.detail.find("Resume, Kill, Allow once or Always allow"), std::string::npos);
  EXPECT_TRUE(suspended.severe);
  const GovernorToast slowed = governor_toast(
      status_with(Json::array({governor_notification_json(1, 1000, 300, "bigjob.exe", "ecoqos")}))
          .notifications[0]);
  EXPECT_EQ(slowed.title, "MyClean slowed bigjob.exe");
  EXPECT_FALSE(slowed.severe);
}

TEST(GovernorToastsTest, FirstStatusOnlySetsTheBaseline) {
  GovernorToasts toasts;
  EXPECT_TRUE(toasts
                  .take(status_with(Json::array(
                      {governor_notification_json(1, 1000, 300, "bigjob.exe", "suspend")})))
                  .empty());
  const std::vector<GovernorToast> next = toasts.take(status_with(
      Json::array({governor_notification_json(1, 1000, 300, "bigjob.exe", "suspend"),
                   governor_notification_json(2, 1010, 400, "other.exe", "priority")})));
  ASSERT_EQ(next.size(), 1U);
  EXPECT_EQ(next[0].title, "MyClean slowed other.exe");
}

// Spec scenarios "Repeated interventions rate-limited" and "New notification after 5 min".
TEST(GovernorToastsTest, OnePerTreePerFiveMinutes) {
  GovernorToasts toasts;
  static_cast<void>(toasts.take(status_with(Json::array())));
  const auto step = [&](std::int64_t id, std::int64_t time, std::string_view kind) {
    return toasts
        .take(status_with(
            Json::array({governor_notification_json(id, time, 300, "bigjob.exe", kind)})))
        .size();
  };
  EXPECT_EQ(step(1, 1000, "priority"), 1U);
  EXPECT_EQ(step(2, 1060, "ecoqos"), 0U);
  EXPECT_EQ(step(3, 1120, "cap_job"), 0U);
  EXPECT_EQ(step(4, 1299, "cap_job"), 0U);
  EXPECT_EQ(step(5, 1300, "cap_job"), 1U) << "5 min after the last toast";
}

TEST(GovernorToastsTest, ASuspendIsAlwaysShown) {
  GovernorToasts toasts;
  static_cast<void>(toasts.take(status_with(Json::array())));
  EXPECT_EQ(toasts
                .take(status_with(Json::array(
                    {governor_notification_json(1, 1000, 300, "bigjob.exe", "cap_job")})))
                .size(),
            1U);
  const std::vector<GovernorToast> suspended = toasts.take(status_with(
      Json::array({governor_notification_json(2, 1010, 300, "bigjob.exe", "suspend")})));
  ASSERT_EQ(suspended.size(), 1U);
  EXPECT_TRUE(suspended[0].severe);
}

TEST(GovernorToastsTest, SeenNotificationsAreNotShownAgainAndARestartedServiceIsNoticed) {
  GovernorToasts toasts;
  static_cast<void>(toasts.take(status_with(Json::array())));
  const Json first = Json::array({governor_notification_json(7, 1000, 300, "bigjob.exe", "kill")});
  EXPECT_EQ(toasts.take(status_with(first)).size(), 1U);
  EXPECT_TRUE(toasts.take(status_with(first)).empty());
  // The service restarted: its ids start at 1 again, its times are later.
  EXPECT_EQ(toasts
                .take(status_with(Json::array(
                    {governor_notification_json(1, 2000, 500, "other.exe", "suspend")})))
                .size(),
            1U);
}

}  // namespace
}  // namespace myclean::tray_model
