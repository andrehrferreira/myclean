// Orphaned polling loops (governor spec, "Orphaned Polling Loops"): which trees are orphaned loop
// trees, how command lines become patterns, and how trees group by pattern.

#include "core/governor/orphans.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/governor/forest.hpp"
#include "core/model/units.hpp"

namespace myclean::governor {
namespace {

using namespace std::chrono_literals;

// 2026-11-01 12:00:00 UTC.
const WallTime noon = std::chrono::sys_days{std::chrono::November / 1 / 2026} + 12h;

// A process table built row by row; `age` is how long before noon a process was created.
class Table {
 public:
  std::size_t add(std::uint32_t pid, std::uint32_t parent_pid, const std::string& image,
                  std::chrono::minutes age, std::optional<std::string> command_line = std::nullopt,
                  bool protected_process = false) {
    OrphanProcess process{.key = {.pid = pid, .created = noon - age + std::chrono::seconds{pid}},
                          .parent_pid = parent_pid,
                          .image = image,
                          .protected_process = protected_process,
                          .command_line = std::move(command_line)};
    for (std::size_t index = 0; index < rows_.size(); ++index) {
      if (rows_[index].key.pid == parent_pid && rows_[index].key.created < process.key.created) {
        process.parent = index;
      }
    }
    rows_.push_back(std::move(process));
    return rows_.size() - 1;
  }

  [[nodiscard]] std::vector<OrphanProcess>& rows() { return rows_; }

 private:
  std::vector<OrphanProcess> rows_;
};

std::vector<std::uint32_t> pids(const std::vector<OrphanProcess>& rows,
                                const std::vector<std::size_t>& indices) {
  std::vector<std::uint32_t> result;
  result.reserve(indices.size());
  for (const std::size_t index : indices) {
    result.push_back(rows[index].key.pid);
  }
  return result;
}

TEST(OrphanLoopsTest, LoopImagesAreShellsInterpretersAndPollingTools) {
  for (const char* image : {"bash.exe", "BASH.EXE", "sh.exe", "pwsh.exe", "powershell.exe",
                            "cmd.exe", "python.exe", "python3.exe", "python3.12.exe", "pythonw.exe",
                            "node.exe", "tail.exe", "sleep.exe", "timeout.exe"}) {
    EXPECT_TRUE(loop_image(image)) << image;
  }
  for (const char* image : {"chrome.exe", "code.exe", "pythonista.exe", "nodemon-gui.exe",
                            "explorer.exe", "myclean-svc.exe", "cargo.exe"}) {
    EXPECT_FALSE(loop_image(image)) << image;
  }
}

TEST(OrphanLoopsTest, PatternsDropWhatChangesBetweenSessions) {
  // A quoted program path, kept out of the macro (a raw string with quotes in a macro argument).
  const std::string quoted = R"("C:\Program Files\Git\usr\bin\bash.exe" )"
                             R"(/c/Users/owner/.claude/watch_channel.sh --pid 41236)";
  EXPECT_EQ(normalize_command_line(quoted), "bash /c/users/owner/.claude/watch_channel.sh --pid #");
  EXPECT_EQ(normalize_command_line(R"(python.exe  C:\Temp\35236526-9535-4170-aa9a-ae9334be16d9\)"
                                   R"(collab.py --since=1730000000)"),
            normalize_command_line(R"(PYTHON.EXE C:\Temp\00a1b2c3-1111-2222-3333-444455556666\)"
                                   R"(collab.py   --since=1730099999)"));
  EXPECT_NE(normalize_command_line("bash watch_channel.sh"),
            normalize_command_line("bash other_loop.sh"));
  EXPECT_EQ(normalize_command_line(std::string(1000, 'a')).size(), max_pattern_length);
  EXPECT_EQ(normalize_command_line(""), "");
}

TEST(OrphanLoopsTest, RootsAreOldLoopProcessesWhoseCreatorIsGone) {
  Table table;
  table.add(10, 4, "claude.exe", 60min);                      // a live session
  table.add(11, 10, "bash.exe", 30min);                       // its loop: the parent lives
  table.add(12, 999, "bash.exe", 30min);                      // its creator exited: an orphan
  table.add(13, 999, "bash.exe", 5min);                       // too young
  table.add(14, 999, "chrome.exe", 30min);                    // not a loop image
  table.add(15, 999, "bash.exe", 30min, std::nullopt, true);  // protected
  // PID 20 was reused by a process created after its child: the child's creator is gone.
  table.add(21, 20, "python.exe", 30min);
  table.add(20, 4, "notepad.exe", 1min);
  const std::vector<std::size_t> roots = orphan_roots(table.rows(), noon, OrphanPolicy{});
  EXPECT_EQ(pids(table.rows(), roots), (std::vector<std::uint32_t>{12, 21}));
  EXPECT_TRUE(orphan_roots(table.rows(), noon, OrphanPolicy{.enabled = false}).empty());
}

TEST(OrphanLoopsTest, TreesGroupByPatternAndLeaveProtectedProcessesOut) {
  Table table;
  for (std::uint32_t pid = 100; pid < 103; ++pid) {
    table.add(pid, 999, "bash.exe", 30min,
              "bash.exe /c/x/watch_channel.sh " + std::to_string(pid * 7));
    table.add(pid + 100, pid, "sleep.exe", 29min, "sleep 5");
  }
  table.add(300, 999, "python.exe", 30min, "python collab.py");
  table.add(301, 300, "title.exe", 29min, std::nullopt, true);  // protected, with a child
  table.add(302, 301, "helper.exe", 28min);
  table.add(400, 999, "bash.exe", 30min);  // command line unknown
  const std::vector<std::size_t> roots = orphan_roots(table.rows(), noon, OrphanPolicy{});
  const std::vector<OrphanGroup> groups = group_orphans(table.rows(), roots);
  ASSERT_EQ(groups.size(), 2U);
  EXPECT_EQ(groups[0].pattern, "bash /c/x/watch_channel.sh #");
  EXPECT_EQ(groups[0].image, "bash.exe");
  ASSERT_EQ(groups[0].trees.size(), 3U);
  EXPECT_EQ(pids(table.rows(), groups[0].trees[0].members), (std::vector<std::uint32_t>{100, 200}));
  EXPECT_EQ(groups[1].pattern, "python collab.py");
  ASSERT_EQ(groups[1].trees.size(), 1U);
  EXPECT_EQ(pids(table.rows(), groups[1].trees[0].members), (std::vector<std::uint32_t>{300}));
}

TEST(OrphanLoopsTest, RulesReadAndWriteAsText) {
  EXPECT_EQ(to_string(OrphanRule::always), "always");
  EXPECT_EQ(parse_orphan_rule("ignore"), OrphanRule::ignore);
  EXPECT_FALSE(parse_orphan_rule("sometimes"));
}

}  // namespace
}  // namespace myclean::governor
