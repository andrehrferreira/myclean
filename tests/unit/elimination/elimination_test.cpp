// Guided freeze elimination over fakes (input-watchdog spec I3): no real process is suspended;
// FakeJobs counts the suspends and resumes.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "actions/actions_test_support.hpp"
#include "core/actions/audit_log.hpp"
#include "core/actions/schema.hpp"
#include "core/alerts/engine.hpp"
#include "core/elimination/keeper.hpp"
#include "core/elimination/pause_action.hpp"
#include "core/elimination/pauses.hpp"
#include "core/elimination/suspects.hpp"
#include "core/elimination/views.hpp"
#include "core/health/analysis.hpp"
#include "core/input/incident.hpp"
#include "core/input/incident_store.hpp"
#include "core/model/action.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/process_control.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/process_collector.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_jobs.hpp"

namespace myclean::elimination {
namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;

const WallTime noon = std::chrono::sys_days{std::chrono::November / 10 / 2026} + 12h;
constexpr std::string_view anydesk = "AnyDesk.exe";
constexpr std::string_view powertoys = "PowerToys.KeyboardManagerEngine.exe";

TEST(RateTest, CountsTheIncidentsInTheSpansAndTheirHours) {
  const std::vector<WallTime> times = {noon - 3h, noon - 1h, noon + 1h};
  const std::vector<Span> spans = {{.from = noon - 2h, .to = noon},
                                   {.from = noon, .to = noon + 2h}};
  const Rate rate = rate_in(times, spans);
  EXPECT_EQ(rate.incidents, 2);
  EXPECT_DOUBLE_EQ(rate.hours, 4.0);
  EXPECT_EQ(rate.per_day(), std::optional<double>(12.0));
  EXPECT_FALSE(Rate{}.per_day().has_value());
}

// Spec "No baseline yet", then the baseline without the time of an earlier pause.
TEST(RateTest, TheBaselineNeedsSevenDaysOfRecordsAndLeavesOutEarlierPauses) {
  const std::vector<WallTime> recent = {noon - 2h};
  EXPECT_FALSE(baseline(recent, noon, {}).has_value());
  EXPECT_FALSE(baseline({}, noon, {}).has_value());
  const std::vector<WallTime> times = {noon - 8 * 24h, noon - 3 * 24h, noon - 2 * 24h - 1h,
                                       noon - 1h};
  const std::optional<Rate> all = baseline(times, noon, {});
  // The one of 8 days ago is before the window.
  EXPECT_EQ(all, (std::optional<Rate>(Rate{.incidents = 3, .hours = 7.0 * 24.0})));
  const std::vector<Span> paused = {{.from = noon - 2 * 24h - 12h, .to = noon - 2 * 24h + 12h}};
  const std::optional<Rate> without = baseline(times, noon, paused);
  EXPECT_EQ(without, (std::optional<Rate>(Rate{.incidents = 2, .hours = 6.0 * 24.0})));
}

TEST(SuspectTest, RankedAsKnownWithTheHookOwnersPausedByTheOwner) {
  const std::vector<Suspect> suspects = ranked_suspects({});
  ASSERT_EQ(suspects.size(), 4U);
  EXPECT_EQ(suspects[0].image, powertoys);
  EXPECT_EQ(suspects[0].id, 1);
  EXPECT_TRUE(suspects[0].hook);
  EXPECT_EQ(suspects[0].mode(), PauseMode::guided);
  EXPECT_EQ(suspects[1].image, anydesk);
  EXPECT_FALSE(suspects[1].hook);
  EXPECT_EQ(suspects[1].mode(), PauseMode::suspend);
  EXPECT_FALSE(suspect_by_id({}, 0).has_value());
  EXPECT_FALSE(suspect_by_id({}, 5).has_value());

  // A suspect running in a slow-keyboard-hook incident counts as a hook owner.
  const input::StoredIncident hooked{
      .incident = {.input_lag = 400ms, .suspects = {"anydesk.exe"}},
      .classification = {.mechanism = input::Mechanism::slow_keyboard_hook}};
  const std::vector<input::StoredIncident> incidents = {hooked};
  EXPECT_TRUE(suspect_by_id(incidents, 2).value_or(Suspect{}).hook);
}

class AlertRecorder final : public health::IAlertSink {
 public:
  AlertRecorder() = default;
  void report(std::span<const alerts::Reading> reported) override {
    readings.insert(readings.end(), reported.begin(), reported.end());
  }
  std::vector<alerts::Reading> readings;
};

class KeeperTest : public ::testing::Test {
 protected:
  KeeperTest() {
    const std::vector<store::MigrationSet> sets = {
        actions::action_migrations(), input::input_migrations(), elimination_migrations()};
    store_ = store::Store::open(folder_.path() / "myclean.db", clock_, sets).value();
    // Freezes recorded for 8 days: 2 a day in the 7 days before noon.
    for (int day = 8; day >= 1; --day) {
      add_incident(noon - std::chrono::days{day} + 2h);
      if (day <= 7) {
        add_incident(noon - std::chrono::days{day} + 5h);
      }
    }
    foreground_ = jobs_.add_process(10, noon - 9h);
    anydesk_ = jobs_.add_process(20, noon - 8h);
    anydesk_child_ = jobs_.add_process(21, noon - 7h);
    service_ = jobs_.add_process(30, noon - 9h);
    view_ = std::make_shared<telemetry::ProcessView>(telemetry::ProcessView{
        .time = noon,
        .rows = {row(foreground_, "AnyDesk.exe", 1), row(anydesk_, "AnyDesk.exe", 1),
                 row(anydesk_child_, "anydesk.exe", 1), row(service_, "AnyDesk.exe", 0),
                 row(jobs_.add_process(40, noon - 9h), "PowerToys.KeyboardManagerEngine.exe", 1),
                 row(jobs_.add_process(50, noon - 9h), "Code.exe", 1)}});
  }

  static telemetry::ProcessRow row(const os::ProcessKey& key, std::string image,
                                   std::uint32_t session) {
    return telemetry::ProcessRow{
        .info = {
            .pid = key.pid, .created = key.created, .image = std::move(image), .session = session}};
  }

  void add_incident(WallTime time) {
    ASSERT_TRUE(
        input::save_incident(*store_, input::Incident{.time = time, .latency = 400ms}).has_value());
  }

  std::unique_ptr<PauseKeeper> keeper() {
    return std::make_unique<PauseKeeper>(
        *store_, clock_,
        KeeperSources{
            .processes = &jobs_,
            .view = [this] { return std::shared_ptr<const telemetry::ProcessView>(view_); },
            .protects = [this](const os::ProcessKey& key) { return key == foreground_; },
            .alerts = &alerts_});
  }

  // Leaves only the rows whose image is not `image`.
  void quit(std::string_view image) {
    std::erase_if(view_->rows, [&](const telemetry::ProcessRow& row) {
      return same_image(row.info.image, image);
    });
  }

  std::vector<actions::LogEntry> log(PlanId plan) {
    std::vector<actions::LogEntry> entries;
    EXPECT_TRUE(store_
                    ->read([&](store::Database& db) -> Result<void> {
                      entries = actions::plan_log(db, plan).value();
                      return {};
                    })
                    .has_value());
    return entries;
  }

  int suspends(const os::ProcessKey& key) const {
    return static_cast<int>(jobs_.process(key).suspend_count);
  }

  fakes::FakeClock& clock() { return clock_; }
  fakes::FakeJobs& jobs() { return jobs_; }
  store::Store& store() { return *store_; }
  telemetry::ProcessView& view() { return *view_; }
  [[nodiscard]] std::shared_ptr<const telemetry::ProcessView> view_ptr() const { return view_; }
  AlertRecorder& alerts() { return alerts_; }
  [[nodiscard]] const os::ProcessKey& foreground_process() const { return foreground_; }
  [[nodiscard]] const os::ProcessKey& anydesk_process() const { return anydesk_; }
  [[nodiscard]] const os::ProcessKey& child_process() const { return anydesk_child_; }
  [[nodiscard]] const os::ProcessKey& service_process() const { return service_; }
  // A folder of its own for a second store (a machine with no freeze recorded).
  [[nodiscard]] std::filesystem::path fresh_folder() const {
    std::filesystem::path fresh = folder_.path() / "fresh";
    std::filesystem::create_directories(fresh);
    return fresh;
  }

 private:
  actions::testing::TempFolder folder_;
  fakes::FakeClock clock_{noon};
  fakes::FakeJobs jobs_;
  std::unique_ptr<store::Store> store_;
  std::shared_ptr<telemetry::ProcessView> view_;
  AlertRecorder alerts_;
  os::ProcessKey foreground_;
  os::ProcessKey anydesk_;
  os::ProcessKey anydesk_child_;
  os::ProcessKey service_;
};

// Spec "Restore at end of period", "One Suspect at a Time", "Rate drops during pause".
TEST_F(KeeperTest, ASuspendPauseSuspendsTheUnprotectedSuspectAndRestoresItAtTheEnd) {
  const std::unique_ptr<PauseKeeper> pauses = keeper();
  const Result<PauseRecord> pause = pauses->pause(PlanId{7}, anydesk, PauseMode::suspend, 24h);
  ASSERT_TRUE(pause.has_value()) << pause.error().message;
  EXPECT_EQ(pause->baseline, (Rate{.incidents = 14, .hours = 168.0}));
  // The owner's session only, never the foreground app or a service.
  EXPECT_EQ(suspends(anydesk_process()), 1);
  EXPECT_EQ(suspends(child_process()), 1);
  EXPECT_EQ(suspends(foreground_process()), 0);
  EXPECT_EQ(suspends(service_process()), 0);
  ASSERT_TRUE(pauses->active()->has_value());

  EXPECT_EQ(pauses->pause(PlanId{8}, powertoys, PauseMode::guided, 24h).error().code,
            Errc::invalid_state);

  clock().advance(23h);
  pauses->tick();
  EXPECT_EQ(suspends(anydesk_process()), 1);
  clock().advance(1h);
  pauses->tick();
  EXPECT_EQ(suspends(anydesk_process()), 0);
  EXPECT_EQ(suspends(child_process()), 0);
  const std::optional<PauseRecord> ended = pauses->record(PlanId{7}).value();
  ASSERT_TRUE(ended.has_value());
  EXPECT_FALSE(ended.value_or(PauseRecord{}).active());
  EXPECT_EQ(ended.value_or(PauseRecord{}).reason, EndReason::period_ended);
  EXPECT_EQ(ended.value_or(PauseRecord{}).during, (Rate{.incidents = 0, .hours = 24.0}));
  EXPECT_EQ(log(PlanId{7}).back().event, "restore");
  EXPECT_EQ(log(PlanId{7}).back().outcome, "period_ended");
  EXPECT_TRUE(alerts().readings.empty());  // only a guided pause reminds the owner
  EXPECT_TRUE(pauses->problem().empty());
}

// Spec "Restore survives a restart": a restarted service resumes what the pause suspended.
TEST_F(KeeperTest, ARestartedServiceResumesASuspendPause) {
  ASSERT_TRUE(keeper()->pause(PlanId{7}, anydesk, PauseMode::suspend, 24h).has_value());
  clock().advance(2h);
  const std::unique_ptr<PauseKeeper> restarted = keeper();
  restarted->recover();
  EXPECT_EQ(suspends(anydesk_process()), 0);
  EXPECT_EQ(restarted->record(PlanId{7}).value().value_or(PauseRecord{}).reason,
            EndReason::service_restarted);
  // A process that exited while paused is forgotten, not resumed.
  ASSERT_TRUE(restarted->pause(PlanId{9}, anydesk, PauseMode::suspend, 24h).has_value());
  jobs().exit_process(child_process(), 0);
  restarted->stop();
  EXPECT_EQ(suspends(anydesk_process()), 0);
  EXPECT_EQ(restarted->record(PlanId{9}).value().value_or(PauseRecord{}).reason,
            EndReason::service_stopped);
}

// Spec "Owner ends a pause early"; a failed resume keeps the pause active for the next tick.
TEST_F(KeeperTest, EndingEarlyResumesAtOnceAndAFailedResumeIsRetried) {
  const std::unique_ptr<PauseKeeper> pauses = keeper();
  ASSERT_TRUE(pauses->pause(PlanId{7}, anydesk, PauseMode::suspend, 24h).has_value());
  jobs().fail_next("resume", Error{.code = Errc::access_denied, .message = "denied"});
  EXPECT_FALSE(pauses->end(PlanId{7}, EndReason::ended_by_owner).has_value());
  EXPECT_TRUE(pauses->active()->has_value());
  ASSERT_TRUE(pauses->end(PlanId{7}, EndReason::ended_by_owner).has_value());
  EXPECT_EQ(suspends(anydesk_process()), 0);
  EXPECT_EQ(suspends(child_process()), 0);
  EXPECT_EQ(pauses->record(PlanId{7}).value().value_or(PauseRecord{}).reason,
            EndReason::ended_by_owner);
  EXPECT_TRUE(pauses->end(PlanId{7}, EndReason::undone).has_value());  // over already
  EXPECT_EQ(pauses->end(PlanId{99}, EndReason::undone).error().code, Errc::not_found);
}

// A suspect that owns a keyboard hook is never suspended: the owner quits it, only the hours it
// does not run count, and the owner is reminded to start it again.
TEST_F(KeeperTest, AGuidedPauseCountsOnlyTheHoursTheSuspectIsNotRunning) {
  const std::unique_ptr<PauseKeeper> pauses = keeper();
  const Result<PauseRecord> pause = pauses->pause(PlanId{8}, powertoys, PauseMode::guided, 24h);
  ASSERT_TRUE(pause.has_value()) << pause.error().message;
  pauses->tick();  // still running: nothing counts
  clock().advance(2h);
  quit(powertoys);  // the owner quits it
  pauses->tick();
  clock().advance(1h);
  add_incident(clock().now());
  pauses->tick();
  clock().advance(5h);
  pauses->stop();  // a clean stop: a guided pause goes on
  EXPECT_TRUE(pauses->active()->has_value());
  const std::unique_ptr<PauseKeeper> restarted = keeper();
  restarted->recover();
  EXPECT_TRUE(restarted->active()->has_value());
  clock().advance(16h);  // the period is over
  restarted->tick();
  const std::optional<PauseRecord> ended = restarted->record(PlanId{8}).value();
  ASSERT_TRUE(ended.has_value());
  EXPECT_EQ(ended.value_or(PauseRecord{}).mode, PauseMode::guided);
  EXPECT_EQ(ended.value_or(PauseRecord{}).reason, EndReason::period_ended);
  ASSERT_TRUE(ended.value_or(PauseRecord{}).during.has_value());
  EXPECT_EQ(ended.value_or(PauseRecord{}).during.value_or(Rate{}).incidents, 1);
  EXPECT_DOUBLE_EQ(ended.value_or(PauseRecord{}).during.value_or(Rate{}).hours,
                   6.0);  // 2 h .. 8 h; the restart's gap is not counted
  for (const telemetry::ProcessRow& row : view().rows) {
    EXPECT_EQ(suspends({.pid = row.info.pid, .created = row.info.created}), 0);
  }
  ASSERT_EQ(alerts().readings.size(), 1U);
  EXPECT_EQ(alerts().readings[0].metric, guided_end_metric);
  EXPECT_EQ(alerts().readings[0].entity, "8");
}

TEST_F(KeeperTest, NoPauseWithoutABaselineOrWithoutASuspendableProcess) {
  const std::unique_ptr<PauseKeeper> pauses = keeper();
  EXPECT_EQ(pauses->pause(PlanId{7}, "Discord.exe", PauseMode::suspend, 24h).error().code,
            Errc::invalid_state);  // not running
  quit(anydesk);
  view().rows.push_back(row(foreground_process(), "AnyDesk.exe", 1));
  EXPECT_EQ(pauses->pause(PlanId{7}, anydesk, PauseMode::suspend, 24h).error().code,
            Errc::invalid_state);  // only the foreground app runs it
  EXPECT_EQ(suspends(foreground_process()), 0);

  const std::vector<store::MigrationSet> sets = {
      actions::action_migrations(), input::input_migrations(), elimination_migrations()};
  const std::unique_ptr<store::Store> fresh =
      store::Store::open(fresh_folder() / "myclean.db", clock(), sets).value();
  PauseKeeper new_machine(*fresh, clock(), KeeperSources{.processes = &jobs()});
  EXPECT_EQ(new_machine.pause(PlanId{7}, powertoys, PauseMode::guided, 24h).error().code,
            Errc::invalid_state);  // a baseline is still being collected
}

TEST_F(KeeperTest, ThePauseActionIsPendingUntilThePauseIsOver) {
  const std::unique_ptr<PauseKeeper> pauses = keeper();
  PauseSuspect action(*pauses, PlanId{7}, std::string(anydesk), PauseMode::suspend, 24h);
  EXPECT_EQ(action.describe().tier, Tier::ask);
  Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview.has_value());
  ASSERT_EQ(preview->items.size(), 1U);
  EXPECT_TRUE(preview->items[0].path.u8string() == u8"pause:AnyDesk.exe");
  const Result<Undo> undo = action.apply(*preview);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  EXPECT_TRUE(undo->reversible());
  const Result<Verification> waiting = action.verify(*undo);
  ASSERT_TRUE(waiting.has_value());
  EXPECT_TRUE(waiting->pending);
  EXPECT_EQ(waiting->metric, "freezes_per_day");
  EXPECT_DOUBLE_EQ(waiting->before, 2.0);
  clock().advance(24h);
  pauses->tick();
  const Result<Verification> measured = action.verify(*undo);
  ASSERT_TRUE(measured.has_value());
  EXPECT_FALSE(measured->pending);
  EXPECT_DOUBLE_EQ(measured->after, 0.0);
  EXPECT_EQ(measured->effect, Effect::better);
  EXPECT_TRUE(action.undo(*undo).has_value());  // over already: nothing to do
}

TEST_F(KeeperTest, AGuidedPauseTheOwnerNeverTookCannotBeCompared) {
  const std::unique_ptr<PauseKeeper> pauses = keeper();
  PauseSuspect action(*pauses, PlanId{8}, std::string(powertoys), PauseMode::guided, 24h);
  const Result<Undo> undo = action.apply(action.preview().value());
  ASSERT_TRUE(undo.has_value());
  clock().advance(24h);
  pauses->tick();  // it ran the whole time
  EXPECT_EQ(action.verify(*undo).error().code, Errc::invalid_state);
}

// Spec "Offer after baseline" and the comparison the Freezes view shows.
TEST_F(KeeperTest, TheAnswerOffersTheTopRunningSuspectAndListsTheComparisons) {
  Json answer = pauses_answer(store(), view_ptr(), clock().now()).value();
  EXPECT_FALSE(answer.at("collecting").get<bool>());
  EXPECT_EQ(answer.at("baseline").at("incidents"), 14);
  EXPECT_EQ(
      answer.at("offer"),
      (Json{{"suspect", 1}, {"image", std::string(powertoys)}, {"mode", "guided"}, {"hours", 24}}));
  EXPECT_EQ(answer.at("suspects")[1].at("running"), true);
  EXPECT_EQ(answer.at("suspects")[2].at("running"), false);

  const std::unique_ptr<PauseKeeper> pauses = keeper();
  ASSERT_TRUE(pauses->pause(PlanId{8}, powertoys, PauseMode::guided, 24h).has_value());
  answer = pauses_answer(store(), view_ptr(), clock().now()).value();
  EXPECT_TRUE(answer.at("offer").is_null());  // one suspect at a time
  EXPECT_EQ(answer.at("active").at("mode"), "guided");
  EXPECT_EQ(answer.at("active").at("running"), true);

  quit(powertoys);
  pauses->tick();
  clock().advance(24h);
  pauses->tick();
  answer = pauses_answer(store(), view_ptr(), clock().now()).value();
  ASSERT_EQ(answer.at("pauses").size(), 1U);
  const Json& compared = answer.at("pauses")[0];
  EXPECT_EQ(compared.at("change"), "lower");
  EXPECT_DOUBLE_EQ(compared.at("baseline").at("per_day").get<double>(), 2.0);
  EXPECT_DOUBLE_EQ(compared.at("during").at("per_day").get<double>(), 0.0);
  EXPECT_EQ(compared.at("end_reason"), "period_ended");
  EXPECT_EQ(answer.at("offer").at("image"), std::string(anydesk));  // the next running suspect

  // No baseline yet: no offer, and the view says so.
  const std::vector<store::MigrationSet> sets = {
      actions::action_migrations(), input::input_migrations(), elimination_migrations()};
  const std::unique_ptr<store::Store> fresh =
      store::Store::open(fresh_folder() / "myclean.db", clock(), sets).value();
  const Json collecting = pauses_answer(*fresh, view_ptr(), clock().now()).value();
  EXPECT_TRUE(collecting.at("collecting").get<bool>());
  EXPECT_TRUE(collecting.at("offer").is_null());
}

}  // namespace
}  // namespace myclean::elimination
