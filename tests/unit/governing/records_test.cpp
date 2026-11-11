// The governor's tables (core/governing/records.hpp).

#include "core/governing/records.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/actions/schema.hpp"
#include "core/governing/keys.hpp"
#include "core/governor/commands.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/orphans.hpp"
#include "core/governor/pressure.hpp"
#include "core/governor/reason.hpp"
#include "core/governor/recovery.hpp"
#include "core/model/actor.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/jobs.hpp"
#include "core/os/process_control.hpp"
#include "core/rules/schema.hpp"
#include "core/store/migrations.hpp"
#include "governing_test_support.hpp"

namespace myclean::governing {
namespace {

using namespace myclean::literals;
using governor::ProcessKey;
using testing::Machine;

TEST(GovernorRecords, MigrationsAreAdditive) {
  const std::vector<store::MigrationSet> sets = {actions::action_migrations(),
                                                 governor_migrations()};
  const Result<void> checked = store::check_additive(sets);
  EXPECT_TRUE(checked.has_value()) << checked.error().message;
  EXPECT_EQ(governor_retention().table, "governor_event");
  EXPECT_EQ(governor_retention().max_age, store::Store::history_retention);
}

TEST(GovernorRecords, EventKindsHaveFixedTexts) {
  for (std::size_t index = 0; index <= static_cast<std::size_t>(EventKind::orphan_rule); ++index) {
    const auto kind = static_cast<EventKind>(index);
    EXPECT_EQ(parse_event_kind(to_string(kind)), kind);
  }
  EXPECT_EQ(to_string(EventKind::notification_limit), "notification_limit");
  EXPECT_FALSE(parse_event_kind("Apply_Step").has_value());
  EXPECT_FALSE(event_kind(governor::CommandKind::persist_suspend).has_value());
  EXPECT_EQ(event_kind(governor::CommandKind::roll_back), EventKind::roll_back);
}

TEST(GovernorRecords, EventRoundTripsWithItsGovernorStep) {
  Machine machine;
  // A creation time with 100 ns ticks survives the store exactly.
  const ProcessKey root{.pid = 4120, .created = testing::noon + WallTime::duration{12345}};
  const GovernorEvent event{.time = testing::noon,
                            .kind = EventKind::apply_step,
                            .root = root,
                            .root_image = "bigjob.exe",
                            .process_class = rules::ProcessClass::normal,
                            .step = governor::Step::cap_job,
                            .reason = governor::Reason::commit,
                            .actor = Actor::governor(),
                            .members = 3,
                            .metrics = governor::PressureSample{.cpu_percent = 97.5,
                                                                .available = 7_GiB,
                                                                .commit = 136_GiB,
                                                                .commit_limit = 150_GiB,
                                                                .hard_faults_per_second = 12.0},
                            .detail = "3 of 3 processes"};
  const Result<std::int64_t> id = save_event(machine.store(), event, true);
  ASSERT_TRUE(id.has_value()) << id.error().message;
  const std::vector<GovernorEvent> loaded = machine.events();
  ASSERT_EQ(loaded.size(), 1U);
  EXPECT_EQ(loaded[0].id, *id);
  EXPECT_EQ(loaded[0].root, root);
  EXPECT_EQ(loaded[0].step, governor::Step::cap_job);
  EXPECT_EQ(loaded[0].reason, governor::Reason::commit);
  EXPECT_EQ(loaded[0].process_class, rules::ProcessClass::normal);
  EXPECT_EQ(loaded[0].members, 3U);
  ASSERT_TRUE(loaded[0].metrics.has_value());
  EXPECT_EQ(loaded[0].metrics.value_or(governor::PressureSample{}).commit, 136_GiB);
  EXPECT_EQ(loaded[0].detail, "3 of 3 processes");
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM action_log WHERE event = 'GovernorStep' AND "
                          "outcome = 'apply_step' AND reason = 'commit' AND detail = 'cap_job "
                          "bigjob.exe pid 4120 class default, 3 processes: 3 of 3 processes'"),
            1);

  ASSERT_TRUE(save_event(machine.store(), GovernorEvent{.kind = EventKind::game_mode}, false));
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM action_log"), 1);
  EXPECT_EQ(machine.events().front().kind, EventKind::game_mode);  // newest first
  EXPECT_FALSE(machine.events().front().root.has_value());
}

TEST(GovernorRecords, SuspendsAreCountedPerProcessInstance) {
  Machine machine;
  const ProcessKey first{.pid = 10, .created = testing::noon};
  const ProcessKey reused{.pid = 10, .created = testing::noon + std::chrono::seconds{5}};
  ASSERT_TRUE(add_suspends(machine.store(), std::vector<ProcessKey>{first, reused}));
  ASSERT_TRUE(add_suspends(machine.store(), std::vector<ProcessKey>{first}));
  Result<std::vector<governor::PersistedSuspend>> suspends = load_suspends(machine.store());
  ASSERT_TRUE(suspends.has_value());
  ASSERT_EQ(suspends->size(), 2U);
  EXPECT_EQ((*suspends)[0].process, first);
  EXPECT_EQ((*suspends)[0].count, 2);
  EXPECT_EQ((*suspends)[1].count, 1);

  ASSERT_TRUE(remove_suspends(machine.store(), first, 1));
  ASSERT_TRUE(remove_suspends(machine.store(), reused, 1));
  suspends = load_suspends(machine.store());
  ASSERT_TRUE(suspends.has_value());
  ASSERT_EQ(suspends->size(), 1U);
  EXPECT_EQ((*suspends)[0].count, 1);
  ASSERT_TRUE(forget_suspend(machine.store(), first));
  EXPECT_TRUE(load_suspends(machine.store())->empty());
}

TEST(GovernorRecords, SavedStatesRoundTripAndAnEmptyOneIsRemoved) {
  Machine machine;
  const SavedState placed{.process = {.pid = 100, .created = testing::noon},
                          .placed = os::PriorityClass::above_normal};
  const SavedState throttled{.process = {.pid = 200, .created = testing::noon},
                             .priority = os::PriorityClass::below_normal,
                             .memory = os::MemoryPriority::normal,
                             .eco_qos = os::EcoQos::off};
  ASSERT_TRUE(save_states(machine.store(), std::vector<SavedState>{placed, throttled}));
  EXPECT_EQ(load_states(machine.store()).value_or(std::vector<SavedState>{}),
            (std::vector<SavedState>{placed, throttled}));

  ASSERT_TRUE(save_states(machine.store(), std::vector<SavedState>{{.process = placed.process}}));
  EXPECT_EQ(load_states(machine.store()).value_or(std::vector<SavedState>{}),
            std::vector<SavedState>{throttled});
  ASSERT_TRUE(forget_states(machine.store()));
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_saved"), 0);
}

TEST(GovernorRecords, JobsAndAllowListPersist) {
  Machine machine;
  const JobRecord job{.ref = {.job_class = "build", .nonce = std::string(32, 'a')},
                      .role = governor::JobRole::class_job,
                      .process_class = rules::ProcessClass::build,
                      .created = testing::noon};
  ASSERT_TRUE(save_job(machine.store(), job));
  Result<std::vector<JobRecord>> jobs = load_jobs(machine.store());
  ASSERT_TRUE(jobs.has_value());
  ASSERT_EQ(jobs->size(), 1U);
  EXPECT_EQ(jobs->front().ref, job.ref);
  EXPECT_EQ(jobs->front().role, governor::JobRole::class_job);
  EXPECT_FALSE(jobs->front().root.has_value());
  ASSERT_TRUE(forget_job(machine.store(), job.ref));
  EXPECT_TRUE(load_jobs(machine.store())->empty());

  ASSERT_TRUE(add_allowed(machine.store(), "bigjob.exe", testing::noon));
  ASSERT_TRUE(add_allowed(machine.store(), "BigJob.EXE", testing::noon));
  Result<std::vector<std::string>> allowed = load_allowed(machine.store());
  ASSERT_TRUE(allowed.has_value());
  EXPECT_EQ(*allowed, std::vector<std::string>{"bigjob.exe"});
}

TEST(GovernorRecords, OrphanRulesPersistOnePerPattern) {
  Machine machine;
  const Result<std::int64_t> first = save_orphan_rule(
      machine.store(), "bash watch.sh #", "bash.exe", governor::OrphanRule::ignore, testing::noon);
  ASSERT_TRUE(first.has_value()) << first.error().message;
  const Result<std::int64_t> again = save_orphan_rule(
      machine.store(), "bash watch.sh #", "bash.exe", governor::OrphanRule::always, testing::noon);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*again, *first) << "the pattern's rule is replaced, not added";
  ASSERT_TRUE(save_orphan_rule(machine.store(), "python collab.py", "python.exe",
                               governor::OrphanRule::ignore, testing::noon));
  Result<std::vector<OrphanRuleRecord>> rules = load_orphan_rules(machine.store());
  ASSERT_TRUE(rules.has_value());
  ASSERT_EQ(rules->size(), 2U);
  EXPECT_EQ(rules->front().pattern, "bash watch.sh #");
  EXPECT_EQ(rules->front().rule, governor::OrphanRule::always);
  EXPECT_EQ(rules->front().decided, testing::noon);
  ASSERT_TRUE(forget_orphan_rule(machine.store(), *first));
  rules = load_orphan_rules(machine.store());
  ASSERT_EQ(rules->size(), 1U);
  EXPECT_EQ(rules->front().pattern, "python collab.py");
}

}  // namespace
}  // namespace myclean::governing
