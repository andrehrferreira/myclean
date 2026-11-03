// Orphaned polling loops in the governor over fakes (governor spec, "Orphaned Polling Loops"):
// proposals, the owner's answers, `always` rules applied to later orphans, observe mode, and
// what is never ended.

#include "core/governor/orphans.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/events/events.hpp"
#include "core/governing/governor.hpp"
#include "core/governing/records.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/policy.hpp"
#include "core/model/result.hpp"
#include "governing_test_support.hpp"

namespace myclean::governing {
namespace {

using Json = nlohmann::json;
using testing::Machine;

constexpr const char* bash = R"(C:\Program Files\Git\usr\bin\bash.exe)";
constexpr const char* session = R"(C:\Users\owner\AppData\Local\claude\claude.exe)";

class OrphanLoops : public ::testing::Test {
 protected:
  void SetUp() override { machine_.start(); }

  [[nodiscard]] Machine& machine() { return machine_; }

  // A loop process under `parent`, started with `command_line`, which the command-line source
  // also answers (for a governor started later).
  governor::ProcessKey loop(std::uint32_t pid, const governor::ProcessKey& parent,
                            std::string command_line, const char* path = bash) {
    const governor::ProcessKey key = machine_.spawn(pid, path, parent, false);
    machine_.command_lines()[pid] = command_line;
    machine_.governor().on_start(events::ProcessStart{
        .process = {.pid = pid, .created = key.created},
        .parent_pid = parent.pid,
        .parent = events::ProcessKey{.pid = parent.pid, .created = parent.created},
        .session = 1,
        .image = path,
        .command_line = std::move(command_line)});
    return key;
  }

  // `count` watch_channel.sh loops of closed sessions: each session started one and exited.
  std::vector<governor::ProcessKey> orphaned_watchers(std::uint32_t first_pid, int count) {
    std::vector<governor::ProcessKey> loops;
    for (int index = 0; index < count; ++index) {
      const std::uint32_t pid = first_pid + static_cast<std::uint32_t>(index * 10);
      const governor::ProcessKey parent = machine_.spawn(pid, session);
      loops.push_back(loop(pid + 1, parent,
                           "\"" + std::string(bash) +
                               "\" /c/Users/owner/.claude/watch_channel.sh " +
                               std::to_string(4000 + index)));
      machine_.exit(parent);
    }
    return loops;
  }

  [[nodiscard]] Json orphans() { return machine_.governor().status().at("orphans"); }

  [[nodiscard]] bool alive(const governor::ProcessKey& key) const {
    return machine_.process(key).alive;
  }

 private:
  Machine machine_;
};

TEST_F(OrphanLoops, ThreeRepeatedOrphansAreProposedOnce) {
  const std::vector<governor::ProcessKey> loops = orphaned_watchers(100, 3);
  machine().second();
  const Json proposals = orphans().at("proposals");
  ASSERT_EQ(proposals.size(), 1U);
  EXPECT_EQ(proposals[0].at("pattern"), "bash /c/users/owner/.claude/watch_channel.sh #");
  EXPECT_EQ(proposals[0].at("image"), "bash.exe");
  EXPECT_EQ(proposals[0].at("trees"), 3);
  EXPECT_EQ(proposals[0].at("processes"), 3);
  const std::int64_t id = proposals[0].at("id").get<std::int64_t>();
  machine().seconds(10);
  EXPECT_EQ(orphans().at("proposals")[0].at("id"), id) << "the same proposal while it lasts";
  for (const governor::ProcessKey& key : loops) {
    EXPECT_TRUE(alive(key)) << "nothing is ended without the owner";
  }
  EXPECT_EQ(machine().count("SELECT COUNT(*) FROM governor_event WHERE kind = 'orphan_loops'"), 1);
}

TEST_F(OrphanLoops, TwoOrphansOrLiveSessionsAreNotProposed) {
  static_cast<void>(orphaned_watchers(100, 2));
  // Three loops whose sessions still run: their parent chain is alive.
  for (std::uint32_t pid = 300; pid < 330; pid += 10) {
    const governor::ProcessKey parent = machine().spawn(pid, session);
    static_cast<void>(
        loop(pid + 1, parent, std::string(bash) + " /c/Users/owner/.claude/live_loop.sh"));
  }
  machine().second();
  EXPECT_TRUE(orphans().at("proposals").empty());
}

TEST_F(OrphanLoops, EndNowEndsEveryTreeOfTheProposalWithTheOwnerAsActor) {
  const std::vector<governor::ProcessKey> loops = orphaned_watchers(100, 3);
  const governor::ProcessKey child = machine().spawn(150, R"(C:\Git\usr\bin\sleep.exe)", loops[0]);
  machine().second();
  const std::int64_t id = orphans().at("proposals")[0].at("id").get<std::int64_t>();
  const Result<std::size_t> ended =
      machine().governor().decide_orphans(id, governor::OrphanDecision::end_now);
  ASSERT_TRUE(ended.has_value()) << ended.error().message;
  EXPECT_EQ(*ended, 3U);
  for (const governor::ProcessKey& key : loops) {
    EXPECT_FALSE(alive(key));
    EXPECT_EQ(machine().process(key).exit_code, kill_exit_code);
  }
  EXPECT_FALSE(alive(child)) << "the loop's own children go with it";
  EXPECT_TRUE(orphans().at("proposals").empty());
  EXPECT_EQ(machine().count("SELECT COUNT(*) FROM governor_event WHERE kind = 'kill' AND "
                            "reason = 'orphaned_loop' AND actor = 'owner'"),
            3);
  EXPECT_EQ(machine().count("SELECT COUNT(*) FROM action_log WHERE event = 'GovernorStep' AND "
                            "reason = 'orphaned_loop' AND actor = 'owner'"),
            3);
  EXPECT_TRUE(orphans().at("rules").empty()) << "end now stores no rule";
}

TEST_F(OrphanLoops, AlwaysEndsThePatternNowAndLaterOrphansByItself) {
  const std::vector<governor::ProcessKey> loops = orphaned_watchers(100, 3);
  machine().second();
  const std::int64_t id = orphans().at("proposals")[0].at("id").get<std::int64_t>();
  const Result<std::size_t> ended =
      machine().governor().decide_orphans(id, governor::OrphanDecision::always);
  ASSERT_TRUE(ended.has_value()) << ended.error().message;
  EXPECT_EQ(*ended, 3U);
  const Json rules = orphans().at("rules");
  ASSERT_EQ(rules.size(), 1U);
  EXPECT_EQ(rules[0].at("rule"), "always");
  EXPECT_EQ(rules[0].at("pattern"), "bash /c/users/owner/.claude/watch_channel.sh #");

  // One later orphan of the pattern is ended without asking, once it is old enough.
  const std::vector<governor::ProcessKey> later = orphaned_watchers(500, 1);
  machine().seconds(11);
  EXPECT_FALSE(alive(later[0]));
  EXPECT_EQ(machine().count("SELECT COUNT(*) FROM governor_event WHERE kind = 'kill' AND "
                            "reason = 'orphaned_loop' AND actor = 'governor'"),
            1);
  EXPECT_EQ(machine().count("SELECT COUNT(*) FROM governor_event WHERE kind = 'orphan_rule'"), 1);
}

TEST_F(OrphanLoops, TheRuleSurvivesARestartAndCanBeForgotten) {
  static_cast<void>(orphaned_watchers(100, 3));
  machine().second();
  const std::int64_t id = orphans().at("proposals")[0].at("id").get<std::int64_t>();
  ASSERT_TRUE(machine().governor().decide_orphans(id, governor::OrphanDecision::ignore));
  machine().seconds(11);
  EXPECT_TRUE(orphans().at("proposals").empty()) << "an ignored pattern is not proposed again";

  machine().crash();
  machine().start();
  const Json rules = orphans().at("rules");
  ASSERT_EQ(rules.size(), 1U);
  EXPECT_EQ(rules[0].at("rule"), "ignore");
  const std::int64_t rule = rules[0].at("id").get<std::int64_t>();
  ASSERT_TRUE(machine().governor().decide_orphans(rule, governor::OrphanDecision::forget));
  EXPECT_TRUE(orphans().at("rules").empty());
  machine().second();
  EXPECT_EQ(orphans().at("proposals").size(), 1U) << "proposed again once forgotten";
  const Result<std::size_t> unknown =
      machine().governor().decide_orphans(rule, governor::OrphanDecision::forget);
  ASSERT_FALSE(unknown.has_value());
  EXPECT_EQ(unknown.error().code, Errc::not_found);
}

TEST_F(OrphanLoops, ObserveModeOnlyRecords) {
  machine().policy().mode = governor::GovernorMode::observe;
  machine().crash();
  machine().start();
  const std::vector<governor::ProcessKey> loops = orphaned_watchers(100, 3);
  machine().second();
  const std::int64_t id = orphans().at("proposals")[0].at("id").get<std::int64_t>();
  ASSERT_TRUE(machine().governor().decide_orphans(id, governor::OrphanDecision::always));
  for (const governor::ProcessKey& key : loops) {
    EXPECT_TRUE(alive(key));
  }
  EXPECT_EQ(machine().count("SELECT COUNT(*) FROM governor_event WHERE kind = 'kill' AND "
                            "detail = 'observe'"),
            3);
  machine().seconds(30);
  EXPECT_EQ(machine().count("SELECT COUNT(*) FROM governor_event WHERE kind = 'kill'"), 3)
      << "an observed end is recorded once, not every scan";
  EXPECT_EQ(machine().count("SELECT COUNT(*) FROM action_log WHERE reason = 'orphaned_loop' AND "
                            "outcome = 'kill'"),
            0);
}

TEST_F(OrphanLoops, TheForegroundProcessIsNeverPartOfAProposal) {
  const std::vector<governor::ProcessKey> loops = orphaned_watchers(100, 3);
  machine().governor().set_user_state(UserState{.foreground_pid = loops[1].pid});
  machine().second();
  EXPECT_TRUE(orphans().at("proposals").empty()) << "two trees are left, fewer than three";
}

TEST_F(OrphanLoops, AnAllowedImageIsNeverProposed) {
  ASSERT_TRUE(add_allowed(machine().store(), "bash.exe", testing::noon));
  machine().crash();
  machine().start();
  static_cast<void>(orphaned_watchers(100, 3));
  machine().second();
  EXPECT_TRUE(orphans().at("proposals").empty());
}

TEST_F(OrphanLoops, MyCleanUnderALoopIsLeftRunning) {
  const std::vector<governor::ProcessKey> loops = orphaned_watchers(100, 3);
  const governor::ProcessKey own =
      machine().spawn(150, R"(C:\Program Files\MyClean\myclean-tray.exe)", loops[0]);
  machine().second();
  EXPECT_EQ(orphans().at("proposals")[0].at("processes"), 3) << "MyClean is not a member";
  const std::int64_t id = orphans().at("proposals")[0].at("id").get<std::int64_t>();
  ASSERT_TRUE(machine().governor().decide_orphans(id, governor::OrphanDecision::end_now));
  EXPECT_FALSE(alive(loops[0]));
  EXPECT_TRUE(alive(own));
}

TEST_F(OrphanLoops, ACommandLineReadLaterStillGroups) {
  // Processes the governor saw only in the process table: their command line is read once.
  for (std::uint32_t pid = 100; pid < 130; pid += 10) {
    const governor::ProcessKey parent = machine().spawn(pid, session, std::nullopt, false);
    const governor::ProcessKey key = machine().spawn(pid + 1, bash, parent, false);
    machine().command_lines()[key.pid] = "bash.exe collab.py --port " + std::to_string(pid);
    machine().exit(parent);
  }
  machine().second();
  const Json proposals = orphans().at("proposals");
  ASSERT_EQ(proposals.size(), 1U);
  EXPECT_EQ(proposals[0].at("pattern"), "bash collab.py --port #");
}

}  // namespace
}  // namespace myclean::governing
