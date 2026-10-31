#include "core/drives/history.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "drives_fakes.hpp"

namespace myclean::drives {
namespace {

using namespace std::chrono_literals;
using testing::optimized;

const WallTime day = std::chrono::sys_days{std::chrono::October / 29 / 2026};

TEST(InterpretOptimizerEvent, ReadsKindFromTheBinaryDataInAnyLanguage) {
  const Optimization retrim =
      interpret(optimized(day, "Tools & Games (E:)", OptimizationKind::retrim))
          .value_or(Optimization{});
  EXPECT_EQ(retrim.drive, 'E');
  EXPECT_EQ(retrim.kind, OptimizationKind::retrim);
  EXPECT_TRUE(retrim.succeeded);
  EXPECT_EQ(retrim.operation, "otimizar novamente");

  const Optimization defrag =
      interpret(optimized(day, "Backup 1 (k:)", OptimizationKind::defrag)).value_or(Optimization{});
  EXPECT_EQ(defrag.drive, 'K');
  EXPECT_EQ(defrag.kind, OptimizationKind::defrag);
}

TEST(InterpretOptimizerEvent, AFailedRunIsKeptAsNotSucceeded) {
  // Event 264, "the user cancelled the operation" (0x89000006).
  const OptimizerEvent cancelled{.time = day,
                                 .id = event_failed,
                                 .operation = "desfragmentação",
                                 .volume = "SO (C:)",
                                 .data = {0x06, 0x00, 0x00, 0x89, 0xED, 0x01, 0x00, 0x00}};
  const Optimization run = interpret(cancelled).value_or(Optimization{});
  EXPECT_EQ(run.kind, OptimizationKind::defrag);
  EXPECT_FALSE(run.succeeded);
}

TEST(InterpretOptimizerEvent, FallsBackToTheEnglishNameAndSkipsOtherEvents) {
  const OptimizerEvent english{
      .time = day, .id = event_optimized, .operation = "retrim", .volume = "Data (D:)"};
  EXPECT_EQ(interpret(english).value_or(Optimization{}).kind, OptimizationKind::retrim);
  OptimizerEvent unknown = english;
  unknown.operation = "slab consolidation";
  EXPECT_EQ(interpret(unknown).value_or(Optimization{}).kind, OptimizationKind::other);

  OptimizerEvent no_letter = english;
  no_letter.volume = R"(\\?\Volume{6c1b2d4e-0000-0000-0000-100000000000}\)";
  EXPECT_FALSE(interpret(no_letter).has_value());
  OptimizerEvent other_event = english;
  other_event.id = 257;
  EXPECT_FALSE(interpret(other_event).has_value());
}

TEST(LastRuns, NewestSuccessfulRunOfEachKindOnTheDrive) {
  std::vector<Optimization> history;
  for (const OptimizerEvent& event :
       {optimized(day - 14 * 24h, "SO (C:)", OptimizationKind::retrim),
        optimized(day, "SO (C:)", OptimizationKind::retrim),
        optimized(day - 7 * 24h, "SO (C:)", OptimizationKind::defrag),
        optimized(day - 21 * 24h, "Backup 1 (K:)", OptimizationKind::defrag)}) {
    history.push_back(interpret(event).value_or(Optimization{}));
  }
  history.push_back(Optimization{
      .time = day + 1h, .drive = 'C', .kind = OptimizationKind::retrim, .succeeded = false});

  const LastRuns c = last_runs(history, 'C');
  EXPECT_EQ(c.retrim, day);
  EXPECT_EQ(c.defrag, day - 7 * 24h);
  EXPECT_EQ(c.log_since, day - 21 * 24h);
  const LastRuns d = last_runs(history, 'D');
  EXPECT_FALSE(d.retrim.has_value());
  EXPECT_FALSE(d.defrag.has_value());
  EXPECT_EQ(d.log_since, day - 21 * 24h);
}

}  // namespace
}  // namespace myclean::drives
