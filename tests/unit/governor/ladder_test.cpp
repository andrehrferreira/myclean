#include "core/governor/ladder.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/governor/commands.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/jobs.hpp"
#include "core/governor/policy.hpp"
#include "core/governor/pressure.hpp"
#include "core/governor/reason.hpp"
#include "core/model/actor.hpp"
#include "core/model/units.hpp"
#include "core/rules/schema.hpp"
#include "fakes/fake_clock.hpp"
#include "ladder_test_support.hpp"

namespace myclean::governor {
namespace {

using namespace myclean::literals;
using rules::ProcessClass;
using std::chrono::minutes;
using std::chrono::seconds;
using testing::ForestBuilder;

// The default policy with every step open to the default class too: most tests below drive a
// `default` tree up the whole ladder to exercise the mechanism.
LadderPolicy full_ladder() {
  LadderPolicy policy;
  policy.cap = {ProcessClass::build, ProcessClass::test, ProcessClass::normal};
  policy.suspend = policy.cap;
  return policy;
}

PressureStatus under(Reason reason) {
  return {.state = PressureState::pressure, .reasons = {reason}, .clearing = false};
}

const PressureStatus cpu_pressure = under(Reason::cpu);
// Suspend and kill need a memory signal among the reasons.
const PressureStatus memory_pressure = under(Reason::commit);
const PressureStatus calm{};

std::vector<Command> of_kind(const std::vector<Command>& commands, CommandKind kind) {
  std::vector<Command> found;
  std::ranges::copy_if(commands, std::back_inserter(found),
                       [&](const Command& command) { return command.kind == kind; });
  return found;
}

// The apply_step commands of `commands`.
std::vector<Command> applied(const std::vector<Command>& commands) {
  return of_kind(commands, CommandKind::apply_step);
}

std::vector<std::uint32_t> pids(const std::vector<ProcessKey>& keys) {
  std::vector<std::uint32_t> result;
  std::ranges::transform(keys, std::back_inserter(result), &ProcessKey::pid);
  std::ranges::sort(result);
  return result;
}

class LadderTest : public ::testing::Test {
 protected:
  // One second passes, then the engine evaluates the builder's processes under `status`.
  std::vector<Command> second(const PressureStatus& status) {
    clock().advance(seconds{1});
    return tick_now(status);
  }

  std::vector<Command> tick_now(const PressureStatus& status) {
    return engine().tick({.now = clock().monotonic(),
                          .pressure = status,
                          .metrics = metrics(),
                          .processes = builder().build(),
                          .foreground_pid = foreground_,
                          .game_mode = game_mode_,
                          .allowed_images = allowed_});
  }

  // `count` seconds under `status`; every command of them, in order.
  std::vector<Command> seconds_of(int count, const PressureStatus& status) {
    std::vector<Command> all;
    for (int second = 0; second < count; ++second) {
      std::ranges::move(this->second(status), std::back_inserter(all));
    }
    return all;
  }

  [[nodiscard]] PressureSample metrics() const {
    return {.time = clock_.monotonic(),
            .cpu_percent = 97.0,
            .available = 12_GiB,
            .commit = 60_GiB,
            .commit_limit = 150_GiB,
            .hard_faults_per_second = 40.0};
  }

  // A `default` tree: root `pid` (image `image`) with one child pid + 1.
  void add_tree(std::uint32_t pid, const std::string& image, double cpu, Bytes commit,
                ProcessClass process_class = ProcessClass::normal) {
    builder()
        .add(pid, std::nullopt, image, process_class, cpu / 2, Bytes{commit.count() / 2})
        .add(pid + 1, pid, "worker.exe", process_class, cpu / 2, Bytes{commit.count() / 2});
  }

  // Brings the tree of `root` to `step` under memory pressure, one step per cooldown.
  void escalate_to(std::uint32_t root, Step step) {
    for (int guard = 0; guard < 1000 && engine().step_of(builder().key_of(root)) != step; ++guard) {
      static_cast<void>(second(memory_pressure));
    }
    ASSERT_EQ(engine().step_of(builder().key_of(root)), step);
  }

  LadderEngine& engine() { return engine_; }
  ForestBuilder& builder() { return builder_; }
  fakes::FakeClock& clock() { return clock_; }
  std::optional<std::uint32_t>& foreground() { return foreground_; }
  bool& game_mode() { return game_mode_; }
  std::vector<std::string>& allowed() { return allowed_; }

 private:
  fakes::FakeClock clock_{WallTime{}};
  LadderEngine engine_{full_ladder()};
  ForestBuilder builder_;
  std::optional<std::uint32_t> foreground_;
  bool game_mode_ = false;
  std::vector<std::string> allowed_;
};

// ---- Protected Processes Receive No Action --------------------------------------------------

TEST_F(LadderTest, GameNeverReceivesAnAction) {
  builder()
      .add(500, std::nullopt, "game.exe", ProcessClass::game, 95.0, 8_GiB)
      .add(501, 500, "crashhandler.exe", ProcessClass::normal, 1.0, 1_GiB)
      .add(502, 501, "shadercompile.exe", ProcessClass::build, 30.0, 2_GiB);
  add_tree(600, "app.exe", 10.0, 1_GiB);
  const std::vector<Command> commands = seconds_of(120, cpu_pressure);
  ASSERT_FALSE(commands.empty());
  for (const Command& command : commands) {
    EXPECT_EQ(command.root.pid, 600U);
    for (const std::uint32_t pid : pids(command.members)) {
      EXPECT_TRUE(pid == 600 || pid == 601) << pid;
    }
  }
}

TEST_F(LadderTest, SystemProcessIgnored) {
  builder()
      .add(800, std::nullopt, "svchost.exe", ProcessClass::system, 80.0, 1_GiB)
      .add(801, 800, "svchost.exe", ProcessClass::system, 10.0, 1_GiB);
  add_tree(600, "app.exe", 5.0, 1_GiB);
  add_tree(700, "other.exe", 2.0, 1_GiB);
  const std::vector<Command> steps = applied(second(cpu_pressure));
  ASSERT_EQ(steps.size(), 1U);
  EXPECT_EQ(steps[0].root.pid, 600U);
  EXPECT_EQ(pids(steps[0].members), (std::vector<std::uint32_t>{600, 601}));
}

TEST_F(LadderTest, ForegroundProcessIsProtected) {
  add_tree(600, "editor.exe", 60.0, 1_GiB);
  add_tree(700, "other.exe", 10.0, 1_GiB);
  foreground() = 600;
  const std::vector<Command> steps = applied(second(cpu_pressure));
  ASSERT_EQ(steps.size(), 1U);
  // The editor is protected; its child 601 (30 %) is a tree of its own, heavier than 700.
  EXPECT_EQ(steps[0].root.pid, 601U);
  EXPECT_EQ(pids(steps[0].members), (std::vector<std::uint32_t>{601}));
}

// ---- Escalation Ladder ----------------------------------------------------------------------

TEST_F(LadderTest, LadderEscalatesWithCooldown) {
  add_tree(600, "app.exe", 60.0, 2_GiB);
  const std::vector<Command> first = second(cpu_pressure);
  const std::vector<Command> first_steps = applied(first);
  ASSERT_EQ(first_steps.size(), 1U);
  EXPECT_EQ(first_steps[0].step, Step::priority);
  EXPECT_EQ(pids(first_steps[0].members), (std::vector<std::uint32_t>{600, 601}));
  for (int second = 1; second < 10; ++second) {
    EXPECT_TRUE(applied(this->second(cpu_pressure)).empty()) << second;
  }
  const std::vector<Command> next = applied(second(cpu_pressure));
  ASSERT_EQ(next.size(), 1U);
  EXPECT_EQ(next[0].step, Step::ecoqos);
  const std::vector<Command> third = applied(seconds_of(10, cpu_pressure));
  ASSERT_EQ(third.size(), 1U);
  EXPECT_EQ(third[0].step, Step::cap_job);
  ASSERT_TRUE(third[0].cap.has_value());
  const JobLimits cap = third[0].cap.value_or(JobLimits{});
  EXPECT_EQ(cap.cpu_percent, 50);
  EXPECT_EQ(cap.commit, Bytes{(5ULL << 30U) / 2});  // 2 GiB + 25 %
}

TEST_F(LadderTest, SuspendNotifiesTheOwner) {
  add_tree(600, "app.exe", 60.0, 2_GiB);
  escalate_to(600, Step::cap_job);
  const std::vector<Command> commands = seconds_of(10, memory_pressure);
  ASSERT_EQ(commands.size(), 3U);
  EXPECT_EQ(commands[0].kind, CommandKind::persist_suspend);
  EXPECT_EQ(pids(commands[0].members), (std::vector<std::uint32_t>{600, 601}));
  EXPECT_EQ(commands[1].kind, CommandKind::apply_step);
  EXPECT_EQ(commands[1].step, Step::suspend);
  EXPECT_EQ(pids(commands[1].members), (std::vector<std::uint32_t>{600, 601}));
  // Notified although the priority step notified 30 s ago: the owner must be able to act.
  EXPECT_EQ(commands[2].kind, CommandKind::notify);
  EXPECT_EQ(commands[2].step, Step::suspend);
}

TEST_F(LadderTest, KillOffByDefault) {
  add_tree(600, "app.exe", 60.0, 2_GiB);
  escalate_to(600, Step::suspend);
  const std::vector<Command> later = seconds_of(120, under(Reason::commit));
  EXPECT_TRUE(of_kind(later, CommandKind::kill).empty());
  EXPECT_TRUE(of_kind(later, CommandKind::resume).empty());
  EXPECT_EQ(engine().step_of(builder().key_of(600)), Step::suspend);
}

TEST_F(LadderTest, KillWhenOptedIn) {
  LadderPolicy policy = full_ladder();
  policy.kill = {ProcessClass::normal};
  engine().set_policy(policy);
  add_tree(600, "app.exe", 60.0, 2_GiB);
  escalate_to(600, Step::suspend);
  for (int second = 1; second < 10; ++second) {
    EXPECT_TRUE(of_kind(this->second(under(Reason::commit)), CommandKind::kill).empty());
  }
  const std::vector<Command> kills = of_kind(second(under(Reason::commit)), CommandKind::kill);
  ASSERT_EQ(kills.size(), 1U);
  EXPECT_EQ(kills[0].step, Step::kill);
  EXPECT_EQ(kills[0].reason, Reason::commit);
  EXPECT_EQ(kills[0].metrics.commit, 60_GiB);
  EXPECT_EQ(kills[0].actor, Actor::governor());
  EXPECT_EQ(pids(kills[0].members), (std::vector<std::uint32_t>{600, 601}));
  EXPECT_EQ(engine().step_of(builder().key_of(600)), Step::kill);
}

TEST_F(LadderTest, KillIsPerClass) {
  LadderPolicy policy = full_ladder();
  policy.kill = {ProcessClass::build};
  engine().set_policy(policy);
  add_tree(600, "app.exe", 60.0, 2_GiB);
  escalate_to(600, Step::suspend);
  EXPECT_TRUE(of_kind(seconds_of(30, under(Reason::commit)), CommandKind::kill).empty());
}

// Observed on the reference machine: under the default policy the ladder suspended VS Code,
// Chrome and Substance Painter. A program the owner works in stops at EcoQoS: no cap job (its
// commit cap could make allocations fail) and no suspend.
TEST_F(LadderTest, ByDefaultOnlyBuildAndTestTreesAreCappedOrSuspended) {
  engine().set_policy(LadderPolicy{});
  add_tree(600, "app.exe", 60.0, 2_GiB);
  escalate_to(600, Step::ecoqos);
  const std::vector<Command> later = seconds_of(120, memory_pressure);
  EXPECT_TRUE(applied(later).empty());
  EXPECT_TRUE(of_kind(later, CommandKind::persist_suspend).empty());
  EXPECT_EQ(engine().step_of(builder().key_of(600)), Step::ecoqos);

  add_tree(700, "cargo.exe", 90.0, 4_GiB, ProcessClass::build);
  escalate_to(700, Step::suspend);
}

TEST_F(LadderTest, CapWithoutSuspendStopsAtTheCap) {
  LadderPolicy policy;
  policy.cap = {ProcessClass::normal};
  policy.suspend = {};
  engine().set_policy(policy);
  add_tree(600, "app.exe", 60.0, 2_GiB);
  escalate_to(600, Step::cap_job);
  EXPECT_TRUE(of_kind(seconds_of(120, memory_pressure), CommandKind::persist_suspend).empty());
  EXPECT_EQ(engine().step_of(builder().key_of(600)), Step::cap_job);
}

// Priority, EcoQoS and the cap settle a CPU shortage: CPU pressure alone never suspends.
TEST_F(LadderTest, CpuPressureStopsAtTheCap) {
  add_tree(700, "cargo.exe", 90.0, 4_GiB, ProcessClass::build);
  for (int second = 0; second < 30; ++second) {
    static_cast<void>(this->second(cpu_pressure));
  }
  EXPECT_EQ(engine().step_of(builder().key_of(700)), Step::cap_job);
  EXPECT_TRUE(of_kind(seconds_of(300, cpu_pressure), CommandKind::persist_suspend).empty());
  EXPECT_EQ(engine().step_of(builder().key_of(700)), Step::cap_job);
  EXPECT_EQ(of_kind(seconds_of(10, memory_pressure), CommandKind::persist_suspend).size(), 1U);
}

// Observed on the reference machine: while a build ran, the ladder capped Discord, Teams and
// ChatGPT, each heavier than any one compiler. A build or test tree takes the ladder first.
TEST_F(LadderTest, BuildAndTestTreesComeFirst) {
  engine().set_policy(LadderPolicy{});
  add_tree(600, "Discord.exe", 60.0, 2_GiB);
  add_tree(700, "cl.exe", 20.0, 1_GiB, ProcessClass::build);
  add_tree(800, "ctest.exe", 10.0, 1_GiB, ProcessClass::test);
  for (int second = 0; second < 25; ++second) {
    for (const Command& step : applied(this->second(cpu_pressure))) {
      EXPECT_NE(step.root.pid, 600U) << second;
    }
  }
  EXPECT_EQ(engine().step_of(builder().key_of(700)), Step::cap_job);
  EXPECT_FALSE(engine().step_of(builder().key_of(600)).has_value());
  for (int second = 0; second < 60; ++second) {
    static_cast<void>(this->second(cpu_pressure));
  }
  EXPECT_EQ(engine().step_of(builder().key_of(800)), Step::cap_job);
  // Neither can take another step under CPU pressure: only then Discord gets steps, up to EcoQoS.
  EXPECT_EQ(engine().step_of(builder().key_of(600)), Step::ecoqos);
}

TEST_F(LadderTest, LadderActsOnTheHeaviestTreeOneStepAtATime) {
  add_tree(600, "light.exe", 10.0, 1_GiB);
  add_tree(700, "heavy.exe", 60.0, 1_GiB);
  const std::vector<Command> first = applied(second(cpu_pressure));
  ASSERT_EQ(first.size(), 1U);
  EXPECT_EQ(first[0].root.pid, 700U);
  // The heavy tree waits out its cooldown; the ladder does not move to the lighter one.
  EXPECT_TRUE(applied(seconds_of(9, cpu_pressure)).empty());
}

TEST_F(LadderTest, MemoryPressureRanksTreesByCommit) {
  add_tree(600, "cpu-hog.exe", 80.0, 1_GiB);
  add_tree(700, "memory-hog.exe", 5.0, 40_GiB);
  const std::vector<Command> steps = applied(second(under(Reason::commit)));
  ASSERT_EQ(steps.size(), 1U);
  EXPECT_EQ(steps[0].root.pid, 700U);
  EXPECT_EQ(steps[0].reason, Reason::commit);
}

TEST_F(LadderTest, MemoryReasonIsRecordedBeforeCpu) {
  add_tree(600, "app.exe", 80.0, 1_GiB);
  const PressureStatus both{.state = PressureState::pressure,
                            .reasons = {Reason::cpu, Reason::available_memory}};
  const std::vector<Command> steps = applied(second(both));
  ASSERT_EQ(steps.size(), 1U);
  EXPECT_EQ(steps[0].reason, Reason::available_memory);
}

TEST_F(LadderTest, NoStepWhileClearing) {
  add_tree(600, "app.exe", 60.0, 2_GiB);
  const PressureStatus clearing{
      .state = PressureState::pressure, .reasons = {Reason::cpu}, .clearing = true};
  EXPECT_TRUE(seconds_of(30, clearing).empty());
}

TEST_F(LadderTest, GameModeLadderUsesItsCooldown) {
  game_mode() = true;
  add_tree(600, "app.exe", 60.0, 2_GiB);
  ASSERT_EQ(applied(second(cpu_pressure)).size(), 1U);
  EXPECT_TRUE(applied(seconds_of(4, cpu_pressure)).empty());
  const std::vector<Command> next = applied(second(cpu_pressure));
  ASSERT_EQ(next.size(), 1U);
  EXPECT_EQ(next[0].step, Step::ecoqos);
}

TEST_F(LadderTest, CapJobKeepsAtLeast64MiBHeadroom) {
  add_tree(600, "small.exe", 60.0, 100_MiB);
  escalate_to(600, Step::ecoqos);
  const std::vector<Command> steps = applied(seconds_of(10, cpu_pressure));
  ASSERT_EQ(steps.size(), 1U);
  ASSERT_TRUE(steps[0].cap.has_value());
  EXPECT_EQ(steps[0].cap.value_or(JobLimits{}).commit, 164_MiB);
}

TEST_F(LadderTest, StepCarriesTheRootsJobFlag) {
  builder().add(600, std::nullopt, "cargo.exe", ProcessClass::build, 60.0, 1_GiB);
  builder().node(600).in_myclean_job = true;
  const std::vector<Command> steps = applied(second(cpu_pressure));
  ASSERT_EQ(steps.size(), 1U);
  EXPECT_TRUE(steps[0].in_myclean_job);
  EXPECT_EQ(steps[0].process_class, ProcessClass::build);
}

// ---- Every Step Recorded --------------------------------------------------------------------

TEST_F(LadderTest, LadderStepLogged) {
  add_tree(600, "bigjob.exe", 60.0, 2_GiB);
  escalate_to(600, Step::priority);
  const std::vector<Command> steps = applied(seconds_of(10, cpu_pressure));
  ASSERT_EQ(steps.size(), 1U);
  const Command& ecoqos = steps[0];
  EXPECT_EQ(ecoqos.step, Step::ecoqos);
  EXPECT_EQ(to_string(ecoqos.step), "ecoqos");
  EXPECT_EQ(ecoqos.root, builder().key_of(600));
  EXPECT_EQ(ecoqos.root_image, "bigjob.exe");
  EXPECT_EQ(ecoqos.reason, Reason::cpu);
  EXPECT_EQ(ecoqos.actor, Actor::governor());
  EXPECT_EQ(ecoqos.metrics.time, clock().monotonic());
  EXPECT_DOUBLE_EQ(ecoqos.metrics.cpu_percent, 97.0);
  EXPECT_EQ(ecoqos.metrics.available, 12_GiB);
  EXPECT_EQ(ecoqos.metrics.commit, 60_GiB);
}

// ---- Automatic Rollback ---------------------------------------------------------------------

TEST_F(LadderTest, ThrottledTreeRestored) {
  add_tree(600, "app.exe", 60.0, 2_GiB);
  escalate_to(600, Step::cap_job);
  const std::vector<Command> commands = second(calm);
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands[0].kind, CommandKind::roll_back);
  EXPECT_EQ(commands[0].step, Step::cap_job);  // priority, EcoQoS and the cap job
  EXPECT_EQ(commands[0].reason, Reason::pressure_cleared);
  EXPECT_EQ(pids(commands[0].members), (std::vector<std::uint32_t>{600, 601}));
  EXPECT_EQ(engine().step_of(builder().key_of(600)), std::nullopt);
  EXPECT_TRUE(second(calm).empty());
}

TEST_F(LadderTest, SuspendedTreeWaitsForTheOwner) {
  add_tree(600, "app.exe", 60.0, 2_GiB);
  escalate_to(600, Step::suspend);
  const std::vector<Command> commands = seconds_of(600, calm);
  EXPECT_TRUE(of_kind(commands, CommandKind::resume).empty());
  ASSERT_EQ(of_kind(commands, CommandKind::roll_back).size(), 1U);  // steps 1-3 only
  EXPECT_EQ(engine().step_of(builder().key_of(600)), Step::suspend);
  // Its notification remains actionable.
  const std::vector<Command> resumed =
      engine().choose(builder().key_of(600), OwnerChoice::resume, clock().monotonic());
  ASSERT_EQ(resumed.size(), 1U);
  EXPECT_EQ(resumed[0].kind, CommandKind::resume);
}

TEST_F(LadderTest, ThrottledProcessThatBecomesForegroundIsRestored) {
  add_tree(600, "app.exe", 60.0, 2_GiB);
  escalate_to(600, Step::ecoqos);
  foreground() = 600;
  const std::vector<Command> commands = second(cpu_pressure);
  const std::vector<Command> roll_backs = of_kind(commands, CommandKind::roll_back);
  ASSERT_EQ(roll_backs.size(), 1U);
  EXPECT_EQ(roll_backs[0].reason, Reason::protected_process);
  EXPECT_EQ(pids(roll_backs[0].members), (std::vector<std::uint32_t>{600}));
  EXPECT_EQ(roll_backs[0].step, Step::ecoqos);
}

TEST_F(LadderTest, ProcessJoiningAThrottledTreeGetsItsSteps) {
  add_tree(600, "app.exe", 60.0, 2_GiB);
  escalate_to(600, Step::ecoqos);
  builder().add(602, 600, "late-child.exe");
  const std::vector<Command> steps = applied(second(cpu_pressure));
  ASSERT_EQ(steps.size(), 2U);
  EXPECT_EQ(steps[0].step, Step::priority);
  EXPECT_EQ(steps[1].step, Step::ecoqos);
  for (const Command& step : steps) {
    EXPECT_EQ(step.reason, Reason::joined_tree);
    EXPECT_EQ(pids(step.members), (std::vector<std::uint32_t>{602}));
  }
}

TEST_F(LadderTest, StepsFollowMembersWhenTheRootExits) {
  add_tree(600, "app.exe", 60.0, 2_GiB);
  escalate_to(600, Step::ecoqos);
  builder().remove(600);                      // 601 becomes the root of its own tree
  EXPECT_TRUE(second(cpu_pressure).empty());  // 601 carries the cooldown too
  EXPECT_EQ(engine().step_of(builder().key_of(601)), Step::ecoqos);
  const std::vector<Command> commands = second(calm);
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands[0].kind, CommandKind::roll_back);
  EXPECT_EQ(commands[0].root.pid, 601U);
  EXPECT_EQ(pids(commands[0].members), (std::vector<std::uint32_t>{601}));
}

// ---- Rate-Limited Owner Notifications -------------------------------------------------------

TEST_F(LadderTest, RepeatedInterventionsRateLimited) {
  add_tree(600, "app.exe", 60.0, 2_GiB);
  const std::vector<Command> commands = seconds_of(25, cpu_pressure);
  ASSERT_EQ(applied(commands).size(), 3U);  // priority, EcoQoS, cap job within 2 min
  const std::vector<Command> notices = of_kind(commands, CommandKind::notify);
  ASSERT_EQ(notices.size(), 1U);
  EXPECT_EQ(notices[0].step, Step::priority);
}

TEST_F(LadderTest, AlwaysAllow) {
  add_tree(600, "bigjob.exe", 60.0, 2_GiB);
  escalate_to(600, Step::suspend);
  const std::vector<Command> commands =
      engine().choose(builder().key_of(600), OwnerChoice::always_allow, clock().monotonic());
  ASSERT_EQ(commands.size(), 3U);
  EXPECT_EQ(commands[0].kind, CommandKind::roll_back);
  EXPECT_EQ(commands[0].step, Step::cap_job);
  EXPECT_EQ(commands[1].kind, CommandKind::resume);
  EXPECT_EQ(pids(commands[1].members), (std::vector<std::uint32_t>{600, 601}));
  EXPECT_EQ(commands[2].kind, CommandKind::add_to_allow_list);
  EXPECT_EQ(commands[2].root_image, "bigjob.exe");
  for (const Command& command : commands) {
    EXPECT_EQ(command.actor, Actor::owner());
    EXPECT_EQ(command.reason, Reason::always_allow);
  }
  // The service adds bigjob.exe to the owner's allow list; a new bigjob.exe gets nothing.
  allowed() = {"bigjob.exe"};
  builder().remove(601).remove(600);
  builder().add(900, std::nullopt, "bigjob.exe", ProcessClass::normal, 90.0, 4_GiB);
  EXPECT_TRUE(seconds_of(60, memory_pressure).empty());
}

// ---- Owner Exemptions Survive Continued Pressure --------------------------------------------

TEST_F(LadderTest, AllowOnceHoldsUnderPressure) {
  add_tree(600, "heavy.exe", 60.0, 2_GiB);
  add_tree(700, "lighter.exe", 20.0, 1_GiB);
  escalate_to(600, Step::suspend);
  const std::vector<Command> released =
      engine().choose(builder().key_of(600), OwnerChoice::allow_once, clock().monotonic());
  ASSERT_EQ(released.size(), 2U);
  EXPECT_EQ(released[0].kind, CommandKind::roll_back);
  EXPECT_EQ(released[1].kind, CommandKind::resume);
  EXPECT_EQ(released[1].reason, Reason::allow_once);
  const std::vector<Command> later = seconds_of(120, memory_pressure);
  ASSERT_FALSE(applied(later).empty());
  for (const Command& command : later) {
    EXPECT_EQ(command.root.pid, 700U);
  }
}

TEST_F(LadderTest, AllowOnceEndsWithTheTree) {
  add_tree(600, "heavy.exe", 60.0, 2_GiB);
  static_cast<void>(second(cpu_pressure));
  static_cast<void>(
      engine().choose(builder().key_of(600), OwnerChoice::allow_once, clock().monotonic()));
  EXPECT_TRUE(applied(seconds_of(30, cpu_pressure)).empty());
  builder().remove(601).remove(600);
  add_tree(800, "heavy.exe", 60.0, 2_GiB);
  const std::vector<Command> steps = applied(second(cpu_pressure));
  ASSERT_EQ(steps.size(), 1U);
  EXPECT_EQ(steps[0].root.pid, 800U);
  EXPECT_EQ(steps[0].step, Step::priority);
}

TEST_F(LadderTest, ResumeBlocksResuspensionFor10Minutes) {
  add_tree(600, "heavy.exe", 60.0, 2_GiB);
  add_tree(700, "lighter.exe", 20.0, 1_GiB);
  escalate_to(600, Step::suspend);
  const std::vector<Command> resumed =
      engine().choose(builder().key_of(600), OwnerChoice::resume, clock().monotonic());  // "12:00"
  ASSERT_EQ(resumed.size(), 1U);
  EXPECT_EQ(resumed[0].kind, CommandKind::resume);
  EXPECT_EQ(resumed[0].actor, Actor::owner());
  EXPECT_EQ(engine().step_of(builder().key_of(600)), Step::cap_job);  // lower steps stay

  // Until 12:10 the heaviest tree is never suspended; the ladder works on the next tree.
  const std::vector<Command> until_ten = seconds_of(599, memory_pressure);
  for (const Command& command : until_ten) {
    EXPECT_FALSE(command.root.pid == 600U && command.step == Step::suspend);
  }
  EXPECT_FALSE(applied(until_ten).empty());
  EXPECT_EQ(engine().step_of(builder().key_of(700)), Step::suspend);
  const std::vector<Command> at_ten = applied(second(memory_pressure));
  ASSERT_EQ(at_ten.size(), 1U);
  EXPECT_EQ(at_ten[0].root.pid, 600U);
  EXPECT_EQ(at_ten[0].step, Step::suspend);
}

TEST_F(LadderTest, OwnerKill) {
  add_tree(600, "app.exe", 60.0, 2_GiB);
  escalate_to(600, Step::suspend);
  const std::vector<Command> commands =
      engine().choose(builder().key_of(600), OwnerChoice::kill, clock().monotonic());
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands[0].kind, CommandKind::kill);
  EXPECT_EQ(commands[0].actor, Actor::owner());
  EXPECT_EQ(commands[0].reason, Reason::owner_kill);
  EXPECT_EQ(pids(commands[0].members), (std::vector<std::uint32_t>{600, 601}));
}

TEST_F(LadderTest, ResumeOfATreeThatIsNotSuspendedDoesNothing) {
  add_tree(600, "app.exe", 60.0, 2_GiB);
  static_cast<void>(second(cpu_pressure));
  EXPECT_TRUE(
      engine().choose(builder().key_of(600), OwnerChoice::resume, clock().monotonic()).empty());
}

// ---- Pause Governor -------------------------------------------------------------------------

TEST_F(LadderTest, PauseFor60Minutes) {
  add_tree(600, "first.exe", 60.0, 2_GiB);
  escalate_to(600, Step::suspend);
  // U = 600 is suspended; T = 700 becomes the heaviest and reaches EcoQoS.
  add_tree(700, "second.exe", 50.0, 1_GiB);
  escalate_to(700, Step::ecoqos);
  const std::vector<Command> released = engine().pause(clock().monotonic(), minutes{60});
  const std::vector<Command> roll_backs = of_kind(released, CommandKind::roll_back);
  const std::vector<Command> resumes = of_kind(released, CommandKind::resume);
  ASSERT_EQ(roll_backs.size(), 2U);
  ASSERT_EQ(resumes.size(), 1U);
  EXPECT_EQ(resumes[0].root.pid, 600U);
  for (const Command& command : released) {
    EXPECT_EQ(command.reason, Reason::pause);
    EXPECT_EQ(command.actor, Actor::owner());
  }
  EXPECT_TRUE(engine().paused(clock().monotonic()));
  EXPECT_TRUE(seconds_of(3599, memory_pressure).empty());
}

TEST_F(LadderTest, PauseExpires) {
  add_tree(600, "app.exe", 60.0, 2_GiB);
  static_cast<void>(engine().pause(clock().monotonic(), minutes{60}));
  EXPECT_TRUE(seconds_of(3599, cpu_pressure).empty());
  const std::vector<Command> steps = applied(second(cpu_pressure));
  ASSERT_EQ(steps.size(), 1U);
  EXPECT_EQ(steps[0].step, Step::priority);
  EXPECT_FALSE(engine().paused(clock().monotonic()));
}

// ---- Fail-Open on Clean Stop ----------------------------------------------------------------

TEST_F(LadderTest, StopReleasesEverything) {
  add_tree(600, "first.exe", 60.0, 2_GiB);
  escalate_to(600, Step::suspend);
  add_tree(700, "second.exe", 50.0, 1_GiB);
  escalate_to(700, Step::cap_job);
  const std::vector<Command> released = engine().stop();
  EXPECT_EQ(of_kind(released, CommandKind::roll_back).size(), 2U);
  const std::vector<Command> resumes = of_kind(released, CommandKind::resume);
  ASSERT_EQ(resumes.size(), 1U);
  EXPECT_EQ(pids(resumes[0].members), (std::vector<std::uint32_t>{600, 601}));
  for (const Command& command : released) {
    EXPECT_EQ(command.reason, Reason::stop);
  }
  EXPECT_EQ(engine().step_of(builder().key_of(600)), std::nullopt);
  EXPECT_TRUE(engine().stop().empty());
}

TEST(OwnerChoiceText, RoundTrips) {
  for (const OwnerChoice choice : {OwnerChoice::resume, OwnerChoice::kill, OwnerChoice::allow_once,
                                   OwnerChoice::always_allow}) {
    EXPECT_EQ(parse_owner_choice(to_string(choice)), choice);
  }
  EXPECT_EQ(to_string(Step::cap_job), "cap_job");
  EXPECT_EQ(parse_step("suspend"), Step::suspend);
  EXPECT_FALSE(parse_step("freeze").has_value());
  EXPECT_EQ(to_string(CommandKind::persist_suspend), "persist_suspend");
}

}  // namespace
}  // namespace myclean::governor
