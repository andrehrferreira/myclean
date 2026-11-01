#include "core/governor/forest.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/units.hpp"
#include "core/rules/schema.hpp"
#include "core/telemetry/process_collector.hpp"
#include "ladder_test_support.hpp"

namespace myclean::governor {
namespace {

using namespace myclean::literals;
using rules::ProcessClass;
using testing::at;
using testing::ForestBuilder;

// The PIDs of the members of the tree rooted at `root_pid`; empty when there is no such tree.
std::vector<std::uint32_t> tree_pids(const Forest& forest, std::uint32_t root_pid) {
  for (const Tree& tree : forest.trees) {
    if (forest.processes[tree.root].key.pid == root_pid) {
      std::vector<std::uint32_t> pids;
      pids.reserve(tree.members.size());
      for (const std::size_t member : tree.members) {
        pids.push_back(forest.processes[member].key.pid);
      }
      std::ranges::sort(pids);
      return pids;
    }
  }
  return {};
}

bool is_protected(const Forest& forest, std::uint32_t pid) {
  for (std::size_t index = 0; index < forest.processes.size(); ++index) {
    if (forest.processes[index].key.pid == pid) {
      return forest.protection[index];
    }
  }
  return false;
}

TEST(Forest, ChildrenOfProtectedProcessesStartTrees) {
  ForestBuilder builder;
  builder.add(4, std::nullopt, "System", ProcessClass::system)
      .add(700, 4, "services.exe", ProcessClass::system)
      .add(800, 700, "svchost.exe", ProcessClass::system)
      .add(900, std::nullopt, "explorer.exe", ProcessClass::system)
      .add(1000, 900, "pwsh.exe")
      .add(1100, 1000, "cargo.exe", ProcessClass::build)
      .add(1200, 1100, "rustc.exe", ProcessClass::build)
      .add(1300, 800, "updater.exe");
  const Forest forest = build_forest(builder.build(), std::nullopt, {});
  ASSERT_EQ(forest.trees.size(), 2U);
  EXPECT_EQ(tree_pids(forest, 1000), (std::vector<std::uint32_t>{1000, 1100, 1200}));
  EXPECT_EQ(tree_pids(forest, 1300), (std::vector<std::uint32_t>{1300}));
}

TEST(Forest, PidReuseNotMistakenForAChild) {
  // PID 4120 was created after its claimed parent 3000 had exited and PID 3000 was reused by a
  // later process: the reused PID's process is created after 4120, so it cannot be its parent.
  ForestBuilder builder;
  builder.add(3000, std::nullopt, "cargo.exe", ProcessClass::build, 50.0, 1_GiB, 500)
      .add(4120, 3000, "worker.exe", ProcessClass::normal, 10.0, 1_GiB, 200);
  const Forest forest = build_forest(builder.build(), std::nullopt, {});
  EXPECT_FALSE(forest.processes[1].parent.has_value());
  EXPECT_EQ(tree_pids(forest, 3000), (std::vector<std::uint32_t>{3000}));
  EXPECT_EQ(tree_pids(forest, 4120), (std::vector<std::uint32_t>{4120}));
}

TEST(Forest, SameCreationTimeIsNoParent) {
  ForestBuilder builder;
  builder.add(10, std::nullopt, "a.exe", ProcessClass::normal, 0.0, Bytes{}, 300)
      .add(11, 10, "b.exe", ProcessClass::normal, 0.0, Bytes{}, 300);
  const Forest forest = build_forest(builder.build(), std::nullopt, {});
  EXPECT_EQ(forest.trees.size(), 2U);
}

TEST(Forest, ProtectedClassesForegroundAndGameDescendants) {
  ForestBuilder builder;
  builder.add(10, std::nullopt, "game.exe", ProcessClass::game)
      .add(11, 10, "crashpad.exe")
      .add(12, 11, "helper.exe", ProcessClass::build)
      .add(20, std::nullopt, "allowed.exe", ProcessClass::allow)
      .add(30, std::nullopt, "editor.exe")
      .add(31, 30, "lsp.exe")
      .add(40, std::nullopt, "BigJob.EXE")
      .add(50, std::nullopt, "normal.exe");
  const std::vector<std::string> allowed = {"bigjob.exe"};
  const Forest forest = build_forest(builder.build(), 30U, allowed);
  for (const std::uint32_t pid : {10U, 11U, 12U, 20U, 30U, 40U}) {
    EXPECT_TRUE(is_protected(forest, pid)) << pid;
  }
  EXPECT_FALSE(is_protected(forest, 31));
  EXPECT_FALSE(is_protected(forest, 50));
  // The foreground editor's child is a root of its own; nothing protected is in any tree.
  ASSERT_EQ(forest.trees.size(), 2U);
  EXPECT_EQ(tree_pids(forest, 31), (std::vector<std::uint32_t>{31}));
  EXPECT_EQ(tree_pids(forest, 50), (std::vector<std::uint32_t>{50}));
}

TEST(Forest, ProtectedChildIsLeftOutOfItsParentsTree) {
  ForestBuilder builder;
  builder.add(10, std::nullopt, "make.exe", ProcessClass::build)
      .add(11, 10, "myclean.exe", ProcessClass::system)
      .add(12, 11, "child.exe")
      .add(13, 10, "cl.exe", ProcessClass::build);
  const Forest forest = build_forest(builder.build(), std::nullopt, {});
  EXPECT_EQ(tree_pids(forest, 10), (std::vector<std::uint32_t>{10, 13}));
  EXPECT_EQ(tree_pids(forest, 12), (std::vector<std::uint32_t>{12}));
}

TEST(Forest, TreeTotalsSumTheMembers) {
  ForestBuilder builder;
  builder.add(10, std::nullopt, "cargo.exe", ProcessClass::build, 5.0, 1_GiB)
      .add(11, 10, "rustc.exe", ProcessClass::build, 20.0, 2_GiB)
      .add(12, 10, "rustc.exe", ProcessClass::build, 30.0, 3_GiB);
  const Forest forest = build_forest(builder.build(), std::nullopt, {});
  ASSERT_EQ(forest.trees.size(), 1U);
  EXPECT_DOUBLE_EQ(forest.trees[0].cpu_percent, 55.0);
  EXPECT_EQ(forest.trees[0].private_bytes, 6_GiB);
  EXPECT_EQ(forest.trees[0].members.front(), forest.trees[0].root);
}

TEST(Forest, NodesFromTelemetryViewKeepTheValidatedLinks) {
  telemetry::ProcessView view;
  view.rows.push_back(
      {.info = {.pid = 10, .created = at(1), .image = "cargo.exe", .private_bytes = 1_GiB},
       .cpu_percent = 12.5});
  view.rows.push_back({.info = {.pid = 11,
                                .parent_pid = 10,
                                .created = at(2),
                                .image = "rustc.exe",
                                .private_bytes = 2_GiB},
                       .parent = 0,
                       .cpu_percent = 40.0});
  const std::vector<ProcessNode> nodes = nodes_from_view(view);
  ASSERT_EQ(nodes.size(), 2U);
  EXPECT_EQ(nodes[1].key.pid, 11U);
  EXPECT_EQ(nodes[1].key.created, at(2));
  EXPECT_EQ(nodes[1].parent, std::optional<std::size_t>{0});
  EXPECT_EQ(nodes[1].image, "rustc.exe");
  EXPECT_DOUBLE_EQ(nodes[1].cpu_percent, 40.0);
  EXPECT_EQ(nodes[1].private_bytes, 2_GiB);
  EXPECT_EQ(nodes[1].process_class, ProcessClass::normal);
  EXPECT_FALSE(nodes[1].in_myclean_job);
}

}  // namespace
}  // namespace myclean::governor
