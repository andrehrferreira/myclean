#include "core/rules/data_files.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/rules/catalog.hpp"
#include "core/rules/schema.hpp"

namespace myclean::rules {
namespace {

using namespace myclean::literals;

// tests/unit/rules/fixtures/data: a data folder in the shipped layout and formats.
const std::filesystem::path fixture_data = std::filesystem::path(MYCLEAN_RULES_FIXTURES) / "data";

TEST(DiskDataFiles, ListsRegularFilesOfOneDirectory) {
  const DiskDataFiles files;
  const Result<std::vector<std::filesystem::path>> listed = files.list(fixture_data);
  ASSERT_TRUE(listed.has_value()) << listed.error().message;
  // The rules/ and kb/ directories are not listed.
  EXPECT_EQ(*listed, std::vector<std::filesystem::path>{fixture_data / "classes.toml"});
}

TEST(DiskDataFiles, MissingDirectoryAndFileAreNotFound) {
  const DiskDataFiles files;
  const Result<std::vector<std::filesystem::path>> listed = files.list(fixture_data / "absent");
  ASSERT_FALSE(listed.has_value());
  EXPECT_EQ(listed.error().code, Errc::not_found);

  const Result<std::string> read = files.read(fixture_data / "absent.toml");
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error().code, Errc::not_found);
}

TEST(DiskDataFiles, LoadsEveryCatalogFromADataFolder) {
  const DiskDataFiles files;
  RuleCatalog rules = make_rule_catalog(fixture_data);
  ClassCatalog classes = make_class_catalog(fixture_data);
  KnowledgeBase kb = make_knowledge_base(fixture_data);

  const std::vector<Error> rule_errors = rules.reload(files);
  const Result<void> class_result = classes.reload(files);
  const std::vector<Error> kb_errors = kb.reload(files);

  ASSERT_TRUE(rule_errors.empty()) << rule_errors.front().message;
  ASSERT_TRUE(class_result.has_value()) << class_result.error().message;
  ASSERT_TRUE(kb_errors.empty()) << kb_errors.front().message;

  EXPECT_EQ(rules.entries().size(), 5U);
  // Cleaning spec, "Shader caches default to ask".
  ASSERT_NE(rules.find("gpu.nvidia.dxcache"), nullptr);
  EXPECT_EQ(rules.find("gpu.nvidia.dxcache")->tier, Tier::ask);

  ASSERT_EQ(classes.classes().matches.size(), 6U);
  EXPECT_EQ(classes.classes().matches.front().id, "system.myclean");
  EXPECT_EQ(classes.classes().build.commit, 96_GiB);
  EXPECT_EQ(classes.classes().test.commit, 64_GiB);

  ASSERT_EQ(kb.entries().size(), 2U);
  ASSERT_NE(kb.find("microsoft.powertoys.keyboard-manager"), nullptr);
  EXPECT_EQ(kb.find("microsoft.powertoys.keyboard-manager")->category, "utility");
}

}  // namespace
}  // namespace myclean::rules
