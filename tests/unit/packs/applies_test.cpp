#include "core/packs/applies.hpp"

#include <regex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "clean_fakes.hpp"
#include "core/model/units.hpp"
#include "core/packs/registry.hpp"
#include "core/packs/schema.hpp"
#include "core/rules/schema.hpp"
#include "pack_fakes.hpp"

namespace myclean::packs {
namespace {

using clean::testing::FakeEnvironment;
using clean::testing::FakeTree;
using testing::FakeMachine;

Condition path(std::string text) {
  Condition condition;
  condition.kind = ConditionKind::path_exists;
  condition.text = std::move(text);
  return condition;
}

Condition app(const std::string& pattern) {
  Condition condition;
  condition.kind = ConditionKind::app_installed;
  condition.text = pattern;
  condition.app = rules::Pattern{
      .text = pattern, .regex = std::regex(pattern, std::regex::ECMAScript | std::regex::icase)};
  return condition;
}

Condition image(std::string name) {
  Condition condition;
  condition.kind = ConditionKind::process_image;
  condition.text = std::move(name);
  return condition;
}

Condition build(std::uint32_t number) {
  Condition condition;
  condition.kind = ConditionKind::os_build_min;
  condition.build = number;
  return condition;
}

class AppliesTest : public ::testing::Test {
 protected:
  AppliesTest() {
    environment_.set(L"LOCALAPPDATA", LR"(C:\Users\owner\AppData\Local)");
    tree_.add_file(LR"(C:\Users\owner\AppData\Local\zig\o\1f\main.obj)", Bytes{4096}, {});
    machine_.apps = {"Zig 0.14.0", "PythonSoftwareFoundation.Python.3.12"};
    machine_.images = {"Code.exe"};
  }

  [[nodiscard]] Probes probes() const {
    return Probes{.machine = &machine_, .tree = &tree_, .environment = &environment_};
  }

  [[nodiscard]] bool holds(const Applicability& applicability) const {
    return applies(applicability, probes());
  }

 private:
  FakeMachine machine_;
  FakeTree tree_;
  FakeEnvironment environment_;
};

TEST_F(AppliesTest, PathExistsExpandsVariablesAndWildcards) {
  EXPECT_TRUE(holds({.all = {path(R"(%LOCALAPPDATA%\zig)")}, .any = {}}));
  EXPECT_TRUE(holds({.all = {path(R"(%LOCALAPPDATA%\z?g\*\1f)")}, .any = {}}));
  EXPECT_FALSE(holds({.all = {path(R"(%LOCALAPPDATA%\ollama)")}, .any = {}}));
  EXPECT_FALSE(holds({.all = {path(R"(%OLLAMA_MODELS%\blobs)")}, .any = {}}));  // not set
}

TEST_F(AppliesTest, AppsProcessesAndBuild) {
  EXPECT_TRUE(holds({.all = {app("^zig ")}, .any = {}}));
  EXPECT_TRUE(holds({.all = {app(R"(^PythonSoftwareFoundation\.Python\.)")}, .any = {}}));
  EXPECT_FALSE(holds({.all = {app("^Ollama")}, .any = {}}));
  EXPECT_TRUE(holds({.all = {image("code.EXE")}, .any = {}}));
  EXPECT_FALSE(holds({.all = {image("ollama.exe")}, .any = {}}));
  EXPECT_TRUE(holds({.all = {build(26100)}, .any = {}}));
  EXPECT_FALSE(holds({.all = {build(26101)}, .any = {}}));
}

TEST_F(AppliesTest, AllAndAnyCombine) {
  EXPECT_TRUE(holds({}));
  EXPECT_TRUE(holds({.all = {}, .any = {app("^Ollama"), image("Code.exe")}}));
  EXPECT_FALSE(holds({.all = {}, .any = {app("^Ollama"), image("ollama.exe")}}));
  EXPECT_FALSE(holds({.all = {build(30000)}, .any = {image("Code.exe")}}));
  EXPECT_TRUE(holds({.all = {build(19041), app("^Zig")}, .any = {image("Code.exe")}}));
}

Pack pack(const std::string& id, Applicability applicability) {
  Pack result;
  result.manifest.id = id;
  result.manifest.applies_when = std::move(applicability);
  rules::CleaningRule rule;
  rule.id = id + "/cache";
  result.clean.push_back(rule);
  rules::ClassMatch match;
  match.id = id + "/build";
  result.classes.push_back(match);
  RelocationRecipe recipe;
  recipe.id = id + "/home";
  result.relocate.push_back(recipe);
  return result;
}

TEST_F(AppliesTest, TheRegistryHoldsTheContentOfApplyingPacksInOrder) {
  const std::vector<Pack> packs = {pack("zig", {.all = {}, .any = {app("^Zig")}}),
                                   pack("ollama", {.all = {}, .any = {app("^Ollama")}}),
                                   pack("vscode", {})};
  const Registry here = make_registry(packs, probes());
  EXPECT_EQ(here.packs, (std::vector<std::string>{"zig", "vscode"}));
  ASSERT_EQ(here.clean.size(), 2U);
  EXPECT_EQ(here.clean[0].id, "zig/cache");
  EXPECT_EQ(here.clean[1].id, "vscode/cache");
  ASSERT_EQ(here.classes.matches.size(), 2U);
  EXPECT_EQ(here.classes.matches[1].id, "vscode/build");
  EXPECT_EQ(here.relocate.size(), 2U);

  const Registry all = make_registry(packs);
  EXPECT_EQ(all.packs.size(), 3U);
  EXPECT_EQ(all.clean.size(), 3U);
}

}  // namespace
}  // namespace myclean::packs
