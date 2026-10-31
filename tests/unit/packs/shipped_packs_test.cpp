#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "core/map/attribution.hpp"
#include "core/model/result.hpp"
#include "core/packs/fixtures.hpp"
#include "core/packs/loader.hpp"
#include "core/packs/schema.hpp"

// The packs shipped in data/packs (docs/packs.md). Every pack folder is one test, discovered
// when ctest runs: a new pack is tested without any build change.
namespace myclean::packs {
namespace {

namespace fs = std::filesystem;

const fs::path packs_dir = fs::path{MYCLEAN_DATA_DIR} / "packs";

Version running() {
  return *parse_version(MYCLEAN_VERSION);
}

std::vector<std::string> shipped_pack_ids() {
  std::vector<std::string> ids;
  std::error_code error;
  for (const fs::directory_entry& entry : fs::directory_iterator(packs_dir, error)) {
    if (entry.is_directory(error)) {
      const std::u8string name = entry.path().filename().u8string();
      ids.emplace_back(name.begin(), name.end());
    }
  }
  std::ranges::sort(ids);
  return ids;
}

class ShippedPack : public ::testing::TestWithParam<std::string> {};

// Valid on its own, with at least one fixture, every fixture passing.
TEST_P(ShippedPack, IsValidAndPassesItsFixtures) {
  const Result<Pack> pack =
      load_pack(DiskPackFiles{}, packs_dir / GetParam(), Origin::builtin, running());
  ASSERT_TRUE(pack.has_value()) << pack.error().message;
  const FixtureRun run = run_fixtures(DiskPackFiles{}, *pack);
  for (const std::string& failure : run.failures) {
    ADD_FAILURE() << failure;
  }
  EXPECT_GT(run.fixtures, 0) << "a shipped pack has at least one fixture in tests\\";
}

INSTANTIATE_TEST_SUITE_P(Packs, ShippedPack, ::testing::ValuesIn(shipped_pack_ids()),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                           std::string name = info.param;
                           std::ranges::replace(name, '-', '_');
                           return name;
                         });

TEST(ShippedPacks, LoadTogetherWithoutConflicts) {
  const LoadReport report = load_packs(DiskPackFiles{}, packs_dir, std::nullopt, running());
  for (const Error& error : report.errors) {
    ADD_FAILURE() << error.message;
  }
  EXPECT_EQ(report.packs.size(), shipped_pack_ids().size());
}

// AI tool data (sessions, generated images, models) is the owner's work: no pack cleans it,
// every area of the ai-tools pack is manual review or relocation.
TEST(ShippedPacks, AiToolDataIsNeverCleaned) {
  const Result<Pack> pack =
      load_pack(DiskPackFiles{}, packs_dir / "ai-tools", Origin::builtin, running());
  ASSERT_TRUE(pack.has_value()) << pack.error().message;
  EXPECT_TRUE(pack->clean.empty());
  ASSERT_FALSE(pack->attribution.empty());
  for (const map::Area& area : pack->attribution) {
    EXPECT_TRUE(area.reclaim.kind == map::ReclaimKind::manual_review ||
                area.reclaim.kind == map::ReclaimKind::relocate)
        << area.id;
  }
}

}  // namespace
}  // namespace myclean::packs
