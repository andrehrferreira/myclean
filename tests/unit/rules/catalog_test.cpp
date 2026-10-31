#include "core/rules/catalog.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/rules/schema.hpp"
#include "fake_data_files.hpp"

namespace myclean::rules {
namespace {

const std::filesystem::path data_dir = R"(C:\MyClean\data)";
const std::filesystem::path rules_dir = data_dir / "rules";

std::string rule(std::string_view id, std::string_view tier = "ask") {
  return "[[rule]]\nid = \"" + std::string(id) + "\"\ncategory = \"caches\"\ntier = \"" +
         std::string(tier) + "\"\nscope = \"user\"\nstrategy = \"quarantine\"\n" +
         "targets = [\"cache\"]\n";
}

std::vector<std::string> ids_of(const RuleCatalog& catalog) {
  std::vector<std::string> ids;
  for (const CleaningRule& entry : catalog.entries()) {
    ids.push_back(entry.id);
  }
  return ids;
}

constexpr std::string_view classes_v1 =
    "[[match]]\nid = \"build.cl\"\nclass = \"build\"\nimage = [\"cl.exe\"]\n";
constexpr std::string_view classes_bad_regex =
    "[[match]]\nid = \"build.cl\"\nclass = \"build\"\nimage = [\"cl.exe\"]\n"
    "[[match]]\nid = \"test.jest\"\nclass = \"test\"\ncommand_line = \"jest(\"\n";

class RuleCatalogTest : public ::testing::Test {
 protected:
  FakeDataFiles files_;
  RuleCatalog catalog_ = make_rule_catalog(data_dir);
};

// Store spec, "Invalid rule file rejected whole": rules from other valid files are loaded.
TEST_F(RuleCatalogTest, InvalidFileRejectedWholeOtherFilesLoaded) {
  files_.put(rules_dir / "browsers.toml", rule("browser.chrome.cache") + rule("browser.edge"));
  files_.put(rules_dir / "dev.toml", rule("dev.ok") + "[[rule]]\nid = \"dev.no-tier\"\n");

  const std::vector<Error> errors = catalog_.reload(files_);

  ASSERT_EQ(errors.size(), 1U);
  EXPECT_EQ(errors[0].code, Errc::invalid_data);
  EXPECT_NE(errors[0].message.find("dev.toml: rule #2 'dev.no-tier': field 'category'"),
            std::string::npos)
      << errors[0].message;
  EXPECT_EQ(ids_of(catalog_), (std::vector<std::string>{"browser.chrome.cache", "browser.edge"}));
  EXPECT_EQ(catalog_.find("dev.ok"), nullptr);
}

// Store spec: an invalid file keeps the last valid version of that file in effect.
TEST_F(RuleCatalogTest, InvalidReloadKeepsLastValidVersionOfThatFile) {
  files_.put(rules_dir / "dev.toml", rule("dev.a", "auto"));
  files_.put(rules_dir / "gpu.toml", rule("gpu.nvidia"));
  ASSERT_TRUE(catalog_.reload(files_).empty());

  files_.put(rules_dir / "dev.toml", rule("dev.a", "never") + rule("dev.b"));
  files_.put(rules_dir / "gpu.toml", rule("gpu.nvidia") + rule("gpu.amd"));
  const std::vector<Error> errors = catalog_.reload(files_);

  ASSERT_EQ(errors.size(), 1U);
  EXPECT_EQ(ids_of(catalog_), (std::vector<std::string>{"dev.a", "gpu.nvidia", "gpu.amd"}));
  ASSERT_NE(catalog_.find("dev.a"), nullptr);
  EXPECT_EQ(catalog_.find("dev.a")->tier, Tier::automatic);
}

TEST_F(RuleCatalogTest, ValidReloadReplacesAndRemovedFileDropsItsRules) {
  files_.put(rules_dir / "dev.toml", rule("dev.a"));
  files_.put(rules_dir / "gpu.toml", rule("gpu.nvidia"));
  ASSERT_TRUE(catalog_.reload(files_).empty());

  files_.put(rules_dir / "dev.toml", rule("dev.b"));
  files_.erase(rules_dir / "gpu.toml");
  EXPECT_TRUE(catalog_.reload(files_).empty());

  EXPECT_EQ(ids_of(catalog_), std::vector<std::string>{"dev.b"});
}

TEST_F(RuleCatalogTest, IdRepeatedAcrossFilesRejectsTheLaterFile) {
  files_.put(rules_dir / "a.toml", rule("shared") + rule("a.only"));
  files_.put(rules_dir / "b.toml", rule("b.only") + rule("shared"));

  const std::vector<Error> errors = catalog_.reload(files_);

  ASSERT_EQ(errors.size(), 1U);
  EXPECT_NE(errors[0].message.find(R"(b.toml: entry 'shared' repeats an id defined in )"
                                   R"(C:\MyClean\data\rules\a.toml)"),
            std::string::npos)
      << errors[0].message;
  EXPECT_EQ(ids_of(catalog_), (std::vector<std::string>{"shared", "a.only"}));
}

TEST_F(RuleCatalogTest, OnlyTomlFilesAreRead) {
  files_.put(rules_dir / "dev.toml", rule("dev.a"));
  files_.put(rules_dir / "README.md", "not a rule file");
  files_.put(rules_dir / "dev.toml.bak", "[[rule]]\nbroken");

  EXPECT_TRUE(catalog_.reload(files_).empty());
  EXPECT_EQ(ids_of(catalog_), std::vector<std::string>{"dev.a"});
}

TEST_F(RuleCatalogTest, MissingDirectoryChangesNothing) {
  files_.put(rules_dir / "dev.toml", rule("dev.a"));
  ASSERT_TRUE(catalog_.reload(files_).empty());

  files_.erase(rules_dir / "dev.toml");
  const std::vector<Error> errors = catalog_.reload(files_);

  ASSERT_EQ(errors.size(), 1U);
  EXPECT_EQ(errors[0].code, Errc::not_found);
  EXPECT_EQ(ids_of(catalog_), std::vector<std::string>{"dev.a"});
}

// Store spec and governor spec: "Invalid reload keeps previous version".
TEST(ClassCatalog, MalformedRegexReloadKeepsPreviousClasses) {
  FakeDataFiles files;
  ClassCatalog catalog = make_class_catalog(data_dir);
  files.put(data_dir / "classes.toml", std::string(classes_v1));
  ASSERT_TRUE(catalog.reload(files).has_value());

  files.put(data_dir / "classes.toml", std::string(classes_bad_regex));
  const Result<void> reloaded = catalog.reload(files);

  ASSERT_FALSE(reloaded.has_value());
  EXPECT_EQ(reloaded.error().code, Errc::invalid_data);
  EXPECT_NE(reloaded.error().message.find(
                "classes.toml: match #2 'test.jest': field 'command_line' is not a valid"),
            std::string::npos)
      << reloaded.error().message;
  ASSERT_EQ(catalog.classes().matches.size(), 1U);
  EXPECT_EQ(catalog.classes().matches[0].id, "build.cl");
}

TEST(ClassCatalog, MissingFileLeavesEveryProcessDefault) {
  const FakeDataFiles files;
  ClassCatalog catalog = make_class_catalog(data_dir);

  const Result<void> reloaded = catalog.reload(files);

  ASSERT_FALSE(reloaded.has_value());
  EXPECT_EQ(reloaded.error().code, Errc::not_found);
  EXPECT_TRUE(catalog.classes().matches.empty());
}

// Store spec, "Valid knowledge-base file loaded"; trust spec, "Valid knowledge base".
TEST(KnowledgeBase, EveryEntryOfAValidFileIsAvailable) {
  FakeDataFiles files;
  KnowledgeBase kb = make_knowledge_base(data_dir);
  files.put(data_dir / "kb" / "processes.json", R"({ "entries": [
    { "id": "windows.svchost", "image": "svchost.exe", "description": "Hosts Windows services.",
      "category": "system", "needed_at_startup": true, "safe_to_remove": false },
    { "id": "discord", "image": "Discord.exe", "signer": "Discord Inc.",
      "description": "Chat app.", "category": "chat", "needed_at_startup": false,
      "safe_to_remove": true } ] })");

  EXPECT_TRUE(kb.reload(files).empty());

  ASSERT_EQ(kb.entries().size(), 2U);
  ASSERT_NE(kb.find("windows.svchost"), nullptr);
  EXPECT_EQ(kb.find("windows.svchost")->image, "svchost.exe");
  ASSERT_NE(kb.find("discord"), nullptr);
  EXPECT_EQ(kb.find("discord")->signer, "Discord Inc.");
  EXPECT_EQ(kb.find("anydesk"), nullptr);
}

// Trust spec, "Invalid entry": none of the file's entries are used.
TEST(KnowledgeBase, InvalidFileContributesNoEntries) {
  FakeDataFiles files;
  KnowledgeBase kb = make_knowledge_base(data_dir);
  files.put(data_dir / "kb" / "apps.json", R"({ "entries": [
    { "id": "anydesk", "image": "AnyDesk.exe", "category": "remote access",
      "needed_at_startup": false, "safe_to_remove": true } ] })");
  files.put(data_dir / "kb" / "processes.json", R"({ "entries": [
    { "id": "windows.dwm", "image": "dwm.exe", "description": "Draws the desktop.",
      "category": "system", "needed_at_startup": true, "safe_to_remove": false } ] })");

  const std::vector<Error> errors = kb.reload(files);

  ASSERT_EQ(errors.size(), 1U);
  EXPECT_NE(errors[0].message.find("apps.json: entry #1 'anydesk': field 'description'"),
            std::string::npos)
      << errors[0].message;
  ASSERT_EQ(kb.entries().size(), 1U);
  EXPECT_EQ(kb.entries()[0].id, "windows.dwm");
}

}  // namespace
}  // namespace myclean::rules
