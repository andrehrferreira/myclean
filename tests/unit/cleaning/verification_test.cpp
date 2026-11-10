// The verify loop of the cleaning service (optimizer spec, "Before-and-After Verification",
// "Auto-Undo When Worse"): an optimization whose metric is measured later stays applied with its
// verification pending, the worker settles it when the measurement arrives (also after a
// restart), undoes a reversible change that made things worse and raises an alert.
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "cleaning_fixture.hpp"
#include "core/actions/policy.hpp"
#include "core/cleaning/executor.hpp"
#include "core/cleaning/records.hpp"
#include "core/cleaning/service.hpp"
#include "core/model/action.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/startup/boot_times.hpp"
#include "core/startup/diagnostics.hpp"
#include "core/startup/startup.hpp"
#include "core/store/database.hpp"

namespace myclean::cleaning {
namespace {

using namespace std::chrono_literals;
using testing::Json;

// A boot `days` from the test's noon whose logon took `logon`.
startup::BootRecord boot(int days, std::chrono::seconds logon) {
  return startup::BootRecord{.start = testing::noon + std::chrono::days{days} - 4h,
                             .boot = 60s,
                             .main_path = 30s,
                             .post_boot = 20s,
                             .logon = logon};
}

class VerificationTest : public testing::CleaningFixture {
 protected:
  Json get(std::int64_t plan) { return call("plans.get", Json{{"id", plan}}); }

  // Five boots before the change, each logon 30 s; Discord is disabled through an approved plan.
  std::int64_t disable_discord() {
    startup().snapshot.run = {
        {.scope = startup::Scope::user, .name = "Discord", .command = "Update.exe"},
        {.scope = startup::Scope::user, .name = "Steam", .command = "steam.exe -silent"}};
    const std::vector<startup::BootRecord> before = {boot(-5, 30s), boot(-4, 30s), boot(-3, 30s),
                                                     boot(-2, 30s), boot(-1, 30s)};
    EXPECT_TRUE(startup::record_boots(store(), before).has_value());
    start();
    work();
    static_cast<void>(call("startup.list"));
    const std::int64_t id =
        call("plans.propose_startup", Json{{"candidates", {{"ids", {1}}}}}, false)
            .at("plan")
            .get<std::int64_t>();
    approve_all(id);
    work();
    return id;
  }

  void record_boot(int days, std::chrono::seconds logon) {
    const std::vector<startup::BootRecord> after = {boot(days, logon)};
    ASSERT_TRUE(startup::record_boots(store(), after).has_value());
  }

  bool discord_disabled() {
    const startup::Approved* approved =
        startup().approved(startup::Scope::user, startup::ApprovedKey::run, "Discord");
    return approved != nullptr && startup::approved_disabled(approved->bytes);
  }
};

// Spec "Pending verification visible", then "Better or unchanged"; the pending verification
// survives a restart of the service.
TEST_F(VerificationTest, ADisabledItemIsVerifiedAtTheNextLogon) {
  const std::int64_t id = disable_discord();
  EXPECT_TRUE(discord_disabled());
  Json plan = get(id);
  EXPECT_EQ(plan.at("state"), "applied");
  EXPECT_EQ(plan.at("verification").at("result"), "pending");
  EXPECT_EQ(plan.at("verification").at("metric"), "logon_seconds");
  EXPECT_DOUBLE_EQ(plan.at("verification").at("before").get<double>(), 30.0);
  EXPECT_TRUE(plan.at("verification").at("after").is_null());
  const Json listed = call("plans.list");
  EXPECT_EQ(listed.at("plans")[0].at("verification"), "pending");

  // No logon yet: still pending after the next pass.
  clock().advance(CleaningService::verify_interval);
  work();
  EXPECT_EQ(get(id).at("state"), "applied");

  start();  // the service restarts; the next logon took 20 s
  record_boot(1, 20s);
  work();
  plan = get(id);
  EXPECT_EQ(plan.at("state"), "verified");
  EXPECT_EQ(plan.at("verification").at("result"), "better");
  EXPECT_DOUBLE_EQ(plan.at("verification").at("before").get<double>(), 30.0);
  EXPECT_DOUBLE_EQ(plan.at("verification").at("after").get<double>(), 20.0);
  EXPECT_TRUE(discord_disabled());
  EXPECT_TRUE(alerts().readings.empty());
}

// Spec "Reversible and worse": the change is undone, the plan ends rolled_back and the owner is
// told.
TEST_F(VerificationTest, ALongerLogonUndoesTheDisableAndAlerts) {
  const std::int64_t id = disable_discord();
  record_boot(1, 45s);
  clock().advance(CleaningService::verify_interval);
  work();
  const Json plan = get(id);
  EXPECT_EQ(plan.at("state"), "rolled_back");
  EXPECT_EQ(plan.at("verification").at("result"), "worse");
  EXPECT_FALSE(discord_disabled());
  ASSERT_EQ(alerts().readings.size(), 1U);
  EXPECT_EQ(alerts().readings[0].metric, rolled_back_metric);
  EXPECT_EQ(alerts().readings[0].entity, std::to_string(id));
  EXPECT_NE(alerts().readings[0].detail.find("logon_seconds was 30 before the change and 45"),
            std::string::npos);
}

TEST(VerificationReadingsTest, NameWhatHappenedToThePlan) {
  const PlanSpec setting{.kind = PlanKind::system_setting, .title = "Disable startup items"};
  const PlanSpec cleanup{.kind = PlanKind::cleanup, .title = "Delete caches"};
  const Verification worse{.metric = "m", .before = 1.0, .after = 2.0, .effect = Effect::worse};
  EXPECT_TRUE(
      verification_readings(PlanId{3}, setting, actions::RunReport{.state = PlanState::verified})
          .empty());
  EXPECT_TRUE(verification_readings(
                  PlanId{3}, setting,
                  actions::RunReport{.state = PlanState::applied,
                                     .verification = Verification{.metric = "m", .pending = true}})
                  .empty());
  const auto rolled_back = verification_readings(
      PlanId{3}, setting,
      actions::RunReport{.state = PlanState::rolled_back, .verification = worse});
  ASSERT_EQ(rolled_back.size(), 1U);
  EXPECT_EQ(rolled_back[0].metric, rolled_back_metric);
  EXPECT_EQ(rolled_back[0].entity, "3");
  // Spec "Not reversible and worse": kept, the owner is alerted.
  const auto kept =
      verification_readings(PlanId{4}, cleanup,
                            actions::RunReport{.state = PlanState::applied,
                                               .undo = Undo{.not_reversible = "regenerates"},
                                               .verification = worse,
                                               .alert = true});
  ASSERT_EQ(kept.size(), 1U);
  EXPECT_EQ(kept[0].metric, worse_metric);
  EXPECT_NE(kept[0].detail.find("cannot be undone: regenerates"), std::string::npos);
  // A metric that could not be measured: reported for optimizations only.
  const actions::RunReport unmeasured{
      .state = PlanState::applied, .alert = true, .unverified = "no data"};
  ASSERT_EQ(verification_readings(PlanId{5}, setting, unmeasured).size(), 1U);
  EXPECT_EQ(verification_readings(PlanId{5}, setting, unmeasured)[0].metric, unverified_metric);
  EXPECT_TRUE(verification_readings(PlanId{5}, cleanup, unmeasured).empty());
}

}  // namespace
}  // namespace myclean::cleaning
