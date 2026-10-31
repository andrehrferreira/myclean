#include "core/cleanup/dism_cleanup.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/action.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "fakes.hpp"

namespace myclean::cleanup {
namespace {

using testing::FakeRunner;

constexpr const char* dism = R"(C:\Windows\System32\dism.exe)";
constexpr const char* analyze =
    R"(C:\Windows\System32\dism.exe /Online /Cleanup-Image /AnalyzeComponentStore /English)";
constexpr const char* cleanup =
    R"(C:\Windows\System32\dism.exe /Online /Cleanup-Image /StartComponentCleanup /English)";
constexpr std::uint32_t elevation_required = 740;

// Fixtures: analyze-docs-example.txt is the example output of Microsoft's "Determine the Actual
// Size of the WinSxS Folder"; analyze-cleanup-recommended.txt is that output with cleanup
// recommended; analyze-not-elevated.txt was recorded on the owner's PC without elevation
// (exit code 740).
std::string fixture(const std::string& name) {
  const std::ifstream file(std::filesystem::path(MYCLEAN_CLEANUP_FIXTURES) / name,
                           std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

Bytes binary(double amount, std::uint64_t unit) {
  return Bytes{static_cast<std::uint64_t>(std::llround(amount * static_cast<double>(unit)))};
}

TEST(ParseComponentStore, ReadsTheDocumentedExample) {
  const Result<ComponentStore> store = parse_component_store(fixture("analyze-docs-example.txt"));

  ASSERT_TRUE(store) << store.error().message;
  EXPECT_EQ(store->actual_size, binary(4.88, 1ULL << 30U));
  EXPECT_EQ(store->backups_and_disabled_features, binary(506.90, 1ULL << 20U));
  EXPECT_EQ(store->cache_and_temporary_data, binary(279.52, 1ULL << 10U));
  EXPECT_EQ(store->reclaimable_packages, 0);
  EXPECT_FALSE(store->cleanup_recommended);
}

TEST(ParseComponentStore, ReadsARecommendedCleanup) {
  const Result<ComponentStore> store =
      parse_component_store(fixture("analyze-cleanup-recommended.txt"));

  ASSERT_TRUE(store) << store.error().message;
  EXPECT_EQ(store->cache_and_temporary_data, Bytes{});
  EXPECT_EQ(store->reclaimable_packages, 3);
  EXPECT_TRUE(store->cleanup_recommended);
  EXPECT_EQ(store->overhead(), binary(506.90, 1ULL << 20U));
}

TEST(ParseComponentStore, MissingOrMalformedFieldsAreInvalid) {
  EXPECT_EQ(parse_component_store(fixture("analyze-not-elevated.txt")).error().code,
            Errc::invalid_data);
  std::string broken = fixture("analyze-docs-example.txt");
  broken.replace(broken.find("506.90 MB"), 9, "lots");
  EXPECT_EQ(parse_component_store(broken).error().code, Errc::invalid_data);
}

TEST(DismStatus, MapsExitCodesNotText) {
  EXPECT_TRUE(dism_status(0));
  EXPECT_TRUE(dism_status(3010));
  EXPECT_EQ(dism_status(740).error().code, Errc::access_denied);
  EXPECT_EQ(dism_status(5).error().code, Errc::access_denied);
  EXPECT_EQ(dism_status(0x80070005).error().code, Errc::access_denied);
  EXPECT_EQ(dism_status(0x800F0806).error().code, Errc::in_use);
  EXPECT_EQ(dism_status(1223).error().code, Errc::refused);
  EXPECT_EQ(dism_status(87).error().code, Errc::io_error);
  EXPECT_EQ(dism_status(0x800F081F).error().message,
            "DISM failed with exit code 2148468767 (0x800F081F)");
}

TEST(DismCleanup, NotElevatedPreviewIsAccessDenied) {
  FakeRunner runner;
  runner.answer(analyze, fixture("analyze-not-elevated.txt"), elevation_required);
  DismCleanup action{runner, dism, Tier::ask};

  const Result<Preview> preview = action.preview();

  ASSERT_FALSE(preview);
  EXPECT_EQ(preview.error().code, Errc::access_denied);
}

TEST(DismCleanup, NothingRecommendedPreviewsNothingAndRunsNothing) {
  FakeRunner runner;
  runner.answer(analyze, fixture("analyze-docs-example.txt"));
  DismCleanup action{runner, dism, Tier::ask};

  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview);
  EXPECT_TRUE(preview->items.empty());

  ASSERT_TRUE(action.apply(*preview));
  EXPECT_EQ(runner.calls, std::vector<std::string>{analyze});
}

TEST(DismCleanup, ApplyRunsStartComponentCleanupAndVerifyAnalyzesAgain) {
  FakeRunner runner;
  runner.answer(analyze, fixture("analyze-cleanup-recommended.txt"));
  runner.answer(cleanup, "The operation completed successfully.\r\n");
  runner.on_run = [&](const std::string& call) {
    if (call == cleanup) {  // the superseded components are gone
      std::string after = fixture("analyze-docs-example.txt");
      after.replace(after.find("506.90 MB"), 9, "120.00 MB");
      runner.answer(analyze, after);
    }
  };
  DismCleanup action{runner, dism, Tier::ask};
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview);
  ASSERT_EQ(preview->items.size(), 1U);
  EXPECT_EQ(preview->items[0].path, component_store_item());

  const Result<Undo> undo = action.apply(*preview);

  ASSERT_TRUE(undo) << undo.error().message;
  EXPECT_EQ(runner.calls, (std::vector<std::string>{analyze, cleanup}));
  EXPECT_EQ(runner.timeouts.back(), DismCleanup::cleanup_timeout);
  EXPECT_FALSE(undo->reversible());
  const Result<Verification> verification = action.verify(*undo);
  ASSERT_TRUE(verification);
  EXPECT_EQ(verification->metric, "component_store_overhead_bytes");
  EXPECT_EQ(verification->effect, Effect::better);
  EXPECT_EQ(action.undo(*undo).error().code, Errc::not_reversible);
  for (const std::string& call : runner.calls) {
    EXPECT_EQ(call.find("ResetBase"), std::string::npos);
  }
}

TEST(DismCleanup, PendingServicingFailsApply) {
  FakeRunner runner;
  runner.answer(analyze, fixture("analyze-cleanup-recommended.txt"));
  runner.answer(cleanup, "", 0x800F0806);
  DismCleanup action{runner, dism, Tier::ask};
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview);

  const Result<Undo> undo = action.apply(*preview);

  ASSERT_FALSE(undo);
  EXPECT_EQ(undo.error().code, Errc::in_use);
}

TEST(DismCleanup, RunsInTheService) {
  FakeRunner runner;
  const DismCleanup action{runner, dism, Tier::ask};

  EXPECT_EQ(action.describe().context, ExecutionContext::service);
}

}  // namespace
}  // namespace myclean::cleanup
