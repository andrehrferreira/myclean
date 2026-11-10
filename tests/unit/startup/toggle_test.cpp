#include "core/startup/toggle.hpp"

#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/startup/diagnostics.hpp"
#include "core/startup/startup.hpp"
#include "fakes/fake_clock.hpp"
#include "startup_fakes.hpp"

namespace myclean::startup {
namespace {

using namespace std::chrono_literals;
using fakes::FakeClock;
using testing::disabled_value;
using testing::enabled_value;
using testing::FakeStartupSystem;

const WallTime now = std::chrono::sys_days{std::chrono::November / 9 / 2026} + 10h;

class StartupToggleTest : public ::testing::Test {
 protected:
  StartupToggleTest() {
    system_.snapshot.run = {
        {.scope = Scope::user, .name = "Discord", .command = "Update.exe --processStart"},
        {.scope = Scope::user, .name = "Steam", .command = "steam.exe -silent"},
        {.scope = Scope::user, .name = "OneDrive", .command = "OneDrive.exe /background"},
        {.scope = Scope::machine, .wow64 = true, .name = "Adobe CCXProcess", .command = "ccx"},
        {.scope = Scope::user, .once = true, .name = "Uninstall 26", .command = "x"},
        {.scope = Scope::user, .name = "CÃ¢mera Hub", .command = "hub.exe"}};
    system_.snapshot.approved = {
        {.scope = Scope::user, .key = ApprovedKey::run, .name = "Steam", .bytes = enabled_value()},
        {.scope = Scope::user,
         .key = ApprovedKey::run,
         .name = "OneDrive",
         .bytes = disabled_value()}};
    system_.snapshot.tasks = {{.path = "\\ASUS\\Logon", .logon = true, .command = "asus.exe"}};
    system_.snapshot.services = {
        {.name = "asus", .start = {.type = StartType::automatic, .delayed = true}},
        {.name = "MongoDB", .start = {.type = StartType::automatic}}};
  }

  StartupToggle toggle(std::vector<std::string> ids, bool enable = false) {
    return {system_, clock_, std::move(ids), enable};
  }

  // A toggle verified with the boots in boots().
  StartupToggle measured(std::vector<std::string> ids, bool enable = false) {
    return {system_, clock_, std::move(ids), enable,
            [this] { return Result<std::vector<BootRecord>>(boots_); }};
  }

  std::vector<BootRecord>& boots() { return boots_; }

  FakeStartupSystem& system() { return system_; }

 private:
  FakeStartupSystem system_;
  FakeClock clock_{now};
  std::vector<BootRecord> boots_;
};

// A boot `days` from `now` whose logon took `logon_seconds`.
BootRecord boot(int days, int logon_seconds) {
  return BootRecord{.start = now + std::chrono::days{days} - 2h,
                    .boot = 60s,
                    .main_path = 30s,
                    .post_boot = 20s,
                    .logon = std::chrono::seconds{logon_seconds}};
}

TEST_F(StartupToggleTest, DescribesAnAskTierServiceAction) {
  const ActionInfo info = toggle({"run:user:Discord"}).describe();
  EXPECT_EQ(info.tier, Tier::ask);
  EXPECT_EQ(info.context, ExecutionContext::service);
}

TEST_F(StartupToggleTest, PreviewNamesEachItemById) {
  StartupToggle action = toggle({"run:user:Discord", "service:machine:MongoDB"});
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview) << preview.error().message;
  ASSERT_EQ(preview->items.size(), 2U);
  EXPECT_TRUE(preview->items[0].path.u8string() == u8"run:user:Discord");
  EXPECT_TRUE(preview->items[1].path.u8string() == u8"service:machine:MongoDB");
}

TEST_F(StartupToggleTest, PreviewRefusesWhatCannotBeToggled) {
  EXPECT_EQ(toggle({"run:user:Nope"}).preview().error().code, Errc::not_found);
  EXPECT_EQ(toggle({"runonce:user:Uninstall 26"}).preview().error().code, Errc::invalid_argument);
  EXPECT_EQ(toggle({"service:machine:MongoDB"}, true).preview().error().code,
            Errc::invalid_argument);
  EXPECT_EQ(toggle({"run:user:Discord", "run:user:Discord"}).preview().error().code,
            Errc::invalid_argument);
}

// Spec "Run item disabled like Task Manager", "Task disabled", "Service set to Manual".
TEST_F(StartupToggleTest, DisablesThroughNativeMechanismsAndKeepsEverything) {
  StartupToggle action = toggle({"run:user:Discord", "run32:machine:Adobe CCXProcess",
                                 "task:machine:\\ASUS\\Logon", "service:machine:asus"});
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview);
  const Result<Undo> undo = action.apply(*preview);
  ASSERT_TRUE(undo) << undo.error().message;
  EXPECT_TRUE(undo->reversible());
  EXPECT_EQ(undo->items.size(), 4U);

  const Approved* discord = system().approved(Scope::user, ApprovedKey::run, "Discord");
  ASSERT_NE(discord, nullptr);
  EXPECT_EQ(discord->bytes, approved_bytes(true, now));
  ASSERT_NE(system().approved(Scope::machine, ApprovedKey::run32, "Adobe CCXProcess"), nullptr);
  EXPECT_EQ(system().snapshot.run.size(), 6U);  // the Run values stay
  ASSERT_EQ(system().snapshot.tasks.size(), 1U);
  EXPECT_FALSE(system().snapshot.tasks[0].enabled);
  EXPECT_EQ(system().snapshot.services[0].start, (ServiceStart{.type = StartType::manual}));
  EXPECT_EQ(system().snapshot.services.size(), 2U);

  const Result<Verification> verified = action.verify(*undo);
  ASSERT_TRUE(verified) << verified.error().message;
  EXPECT_EQ(verified->metric, "startup_items_changed");
  EXPECT_EQ(verified->after, 4.0);
  EXPECT_EQ(verified->effect, Effect::better);
}

// Spec "Prompt 2 disables only the selection".
TEST_F(StartupToggleTest, ChangesOnlyThePreviewItems) {
  StartupToggle action = toggle({"run:user:Discord", "run:user:Steam"});
  Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview);
  preview->items.pop_back();  // the owner approved Discord only
  ASSERT_TRUE(action.apply(*preview));
  EXPECT_NE(system().approved(Scope::user, ApprovedKey::run, "Discord"), nullptr);
  EXPECT_EQ(system().approved(Scope::user, ApprovedKey::run, "Steam")->bytes, enabled_value());
  EXPECT_EQ(system().writes, 1);
}

// Spec "Delayed-start service restored", "Run item re-enabled": undo restores the exact state,
// the StartupApproved value byte for byte and its absence.
TEST_F(StartupToggleTest, UndoRestoresTheExactStateBefore) {
  StartupToggle action = toggle({"run:user:Discord", "run:user:Steam", "service:machine:asus"});
  const Result<Undo> undo = action.apply(*action.preview());
  ASSERT_TRUE(undo) << undo.error().message;
  ASSERT_TRUE(action.undo(*undo));
  EXPECT_EQ(system().approved(Scope::user, ApprovedKey::run, "Discord"), nullptr);
  EXPECT_EQ(system().approved(Scope::user, ApprovedKey::run, "Steam")->bytes, enabled_value());
  EXPECT_EQ(system().snapshot.services[0].start,
            (ServiceStart{.type = StartType::automatic, .delayed = true}));
}

TEST_F(StartupToggleTest, EnablesADisabledRunItemAndUndoDisablesItAgain) {
  StartupToggle action = toggle({"run:user:OneDrive"}, true);
  const Result<Undo> undo = action.apply(*action.preview());
  ASSERT_TRUE(undo) << undo.error().message;
  EXPECT_EQ(system().approved(Scope::user, ApprovedKey::run, "OneDrive")->bytes, enabled_value());
  ASSERT_TRUE(action.undo(*undo));
  EXPECT_EQ(system().approved(Scope::user, ApprovedKey::run, "OneDrive")->bytes, disabled_value());
}

TEST_F(StartupToggleTest, ItemsAlreadyInTheStateAreLeftAlone) {
  StartupToggle action = toggle({"run:user:OneDrive", "run:user:Discord"});
  const Result<Undo> undo = action.apply(*action.preview());
  ASSERT_TRUE(undo);
  EXPECT_EQ(undo->items.size(), 1U);
  EXPECT_EQ(system().approved(Scope::user, ApprovedKey::run, "OneDrive")->bytes, disabled_value());

  StartupToggle again = toggle({"run:user:OneDrive"});
  EXPECT_EQ(again.apply(*again.preview()).error().code, Errc::invalid_state);
}

TEST_F(StartupToggleTest, AFailedWriteReversesTheChangesMade) {
  StartupToggle action = toggle({"run:user:Discord", "service:machine:asus"});
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview);
  system().fail_write = 2;
  const Result<Undo> undo = action.apply(*preview);
  ASSERT_FALSE(undo);
  EXPECT_EQ(undo.error().code, Errc::access_denied);
  EXPECT_EQ(system().approved(Scope::user, ApprovedKey::run, "Discord"), nullptr);
  EXPECT_EQ(system().snapshot.services[0].start,
            (ServiceStart{.type = StartType::automatic, .delayed = true}));
}

TEST_F(StartupToggleTest, UndoChangesNothingWhenAnItemWasChangedMeanwhile) {
  StartupToggle action = toggle({"run:user:Discord", "service:machine:asus"});
  const Result<Undo> undo = action.apply(*action.preview());
  ASSERT_TRUE(undo);
  system().snapshot.services[0].start = {.type = StartType::disabled};
  const Result<void> undone = action.undo(*undo);
  ASSERT_FALSE(undone);
  EXPECT_EQ(undone.error().code, Errc::invalid_state);
  EXPECT_TRUE(
      approved_disabled(system().approved(Scope::user, ApprovedKey::run, "Discord")->bytes));

  const Result<Verification> verified = action.verify(*undo);
  ASSERT_TRUE(verified);
  EXPECT_EQ(verified->after, 1.0);
  EXPECT_EQ(verified->effect, Effect::worse);
}

// Value names are UTF-8 and byte-exact, through the plan item path and back.
TEST_F(StartupToggleTest, NonAsciiNamesSurviveThePlanItemPath) {
  StartupToggle action = toggle({"run:user:CÃ¢mera Hub"});
  const Result<Undo> undo = action.apply(*action.preview());
  ASSERT_TRUE(undo) << undo.error().message;
  EXPECT_NE(system().approved(Scope::user, ApprovedKey::run, "CÃ¢mera Hub"), nullptr);
}

TEST_F(StartupToggleTest, UndoFailsWhenTheItemIsGone) {
  StartupToggle action = toggle({"task:machine:\\ASUS\\Logon"});
  const Result<Undo> undo = action.apply(*action.preview());
  ASSERT_TRUE(undo);
  system().snapshot.tasks.clear();
  EXPECT_EQ(action.undo(*undo).error().code, Errc::invalid_state);
}

// Spec "Verification recorded": the logon before and after the disable, pending until the next
// logon is measured.
TEST_F(StartupToggleTest, ADisableIsVerifiedWithTheLogonBeforeAndAfterIt) {
  boots() = {boot(-3, 30), boot(-2, 50), boot(-1, 40)};  // median 40 s
  StartupToggle action = measured({"run:user:Discord"});
  const Result<Undo> undo = action.apply(*action.preview());
  ASSERT_TRUE(undo) << undo.error().message;

  const Result<Verification> waiting = action.verify(*undo);
  ASSERT_TRUE(waiting) << waiting.error().message;
  EXPECT_TRUE(waiting->pending);
  EXPECT_EQ(waiting->metric, "logon_seconds");
  EXPECT_DOUBLE_EQ(waiting->before, 40.0);

  boots().push_back(boot(1, 30));  // shorter by more than logon_tolerance
  const Result<Verification> shorter = action.verify(*undo);
  ASSERT_TRUE(shorter) << shorter.error().message;
  EXPECT_FALSE(shorter->pending);
  EXPECT_DOUBLE_EQ(shorter->after, 30.0);
  EXPECT_EQ(shorter->effect, Effect::better);

  boots().back() = boot(1, 42);  // within the tolerance
  EXPECT_EQ(action.verify(*undo)->effect, Effect::unchanged);
  boots().back() = boot(1, 60);
  EXPECT_EQ(action.verify(*undo)->effect, Effect::worse);
}

TEST_F(StartupToggleTest, WithoutALogonBeforeTheChangeOnlyTheChangeIsVerified) {
  boots() = {boot(1, 30)};  // after the change only
  StartupToggle action = measured({"run:user:Discord"});
  const Result<Undo> undo = action.apply(*action.preview());
  ASSERT_TRUE(undo);
  const Result<Verification> verified = action.verify(*undo);
  ASSERT_TRUE(verified) << verified.error().message;
  EXPECT_EQ(verified->metric, "startup_items_changed");
  EXPECT_EQ(verified->effect, Effect::better);
  EXPECT_FALSE(verified->pending);
}

TEST_F(StartupToggleTest, AnItemChangedBackIsWorseWithoutWaitingForALogon) {
  boots() = {boot(-1, 40)};
  StartupToggle action = measured({"run:user:Discord"});
  const Result<Undo> undo = action.apply(*action.preview());
  ASSERT_TRUE(undo);
  system().snapshot.approved.clear();  // Discord is enabled again
  const Result<Verification> verified = action.verify(*undo);
  ASSERT_TRUE(verified);
  EXPECT_EQ(verified->metric, "startup_items_changed");
  EXPECT_EQ(verified->effect, Effect::worse);
}

TEST_F(StartupToggleTest, AnEnableIsNotMeasuredByTheLogon) {
  boots() = {boot(-1, 40)};
  StartupToggle action = measured({"run:user:OneDrive"}, true);
  const Result<Undo> undo = action.apply(*action.preview());
  ASSERT_TRUE(undo) << undo.error().message;
  EXPECT_EQ(action.verify(*undo)->metric, "startup_items_changed");
}

}  // namespace
}  // namespace myclean::startup
