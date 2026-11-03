#include "core/packs/parse.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/packs/schema.hpp"

namespace myclean::packs {
namespace {

const std::filesystem::path file = R"(C:\packs\tool\x.toml)";

constexpr std::string_view manifest_head = R"(id = "tool"
version = "2.0.1"
title = "Tool"
description = "What it covers."
sources = ["https://example.com/docs", "observed on reference machine"]
min_myclean = "0.1.0"
)";

template <typename T>
void expect_error(const Result<T>& result, std::string_view text) {
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, Errc::invalid_data);
  EXPECT_NE(result.error().message.find(text), std::string::npos) << result.error().message;
}

TEST(Version, ParsesMajorMinorPatchOnly) {
  EXPECT_EQ(parse_version("1.20.3"), (Version{.major = 1, .minor = 20, .patch = 3}));
  EXPECT_EQ(to_string(Version{.major = 0, .minor = 1, .patch = 0}), "0.1.0");
  for (const std::string_view bad : {"1.2", "1.2.3.4", "01.2.3", "1.2.x", "1..3", "-1.2.3", ""}) {
    EXPECT_FALSE(parse_version(bad)) << bad;
  }
  EXPECT_LT((Version{.major = 0, .minor = 9, .patch = 9}),
            (Version{.major = 0, .minor = 10, .patch = 0}));
}

TEST(Manifest, ParsesEveryField) {
  const Result<Manifest> manifest = parse_manifest(std::string(manifest_head) + R"(
[applies_when]
all = [{ os_build_min = 19041 }]
any = [{ path_exists = '%LOCALAPPDATA%\tool' }, { app_installed = '^Tool \d' },
       { process_image = "tool.exe" }]
)",
                                                   file);
  ASSERT_TRUE(manifest.has_value()) << manifest.error().message;
  EXPECT_EQ(manifest->id, "tool");
  EXPECT_EQ(manifest->version, (Version{.major = 2, .minor = 0, .patch = 1}));
  EXPECT_EQ(manifest->sources.size(), 2U);
  ASSERT_EQ(manifest->applies_when.all.size(), 1U);
  EXPECT_EQ(manifest->applies_when.all[0].kind, ConditionKind::os_build_min);
  EXPECT_EQ(manifest->applies_when.all[0].build, 19041U);
  ASSERT_EQ(manifest->applies_when.any.size(), 3U);
  EXPECT_EQ(manifest->applies_when.any[0].kind, ConditionKind::path_exists);
  EXPECT_EQ(manifest->applies_when.any[1].kind, ConditionKind::app_installed);
  ASSERT_TRUE(manifest->applies_when.any[1].app);
  EXPECT_EQ(manifest->applies_when.any[2].text, "tool.exe");
}

TEST(Manifest, RejectsInvalidFields) {
  expect_error(parse_manifest("id = \"tool\"\n", file), "field 'version' is missing");
  expect_error(parse_manifest(std::string(manifest_head) + "extra = 1\n", file),
               "field 'extra' is not a known field");
  std::string bad_id(manifest_head);
  bad_id.replace(0, std::string_view(R"(id = "tool")").size(), R"(id = "Tool_1")");
  expect_error(parse_manifest(bad_id, file), "field 'id' must hold only a-z, 0-9 and inner '-'");
  std::string bad_version(manifest_head);
  bad_version.replace(bad_version.find("2.0.1"), 5, "2.0");
  expect_error(parse_manifest(bad_version, file), "field 'version' must be a version");
  expect_error(parse_manifest(std::string(manifest_head) + "[applies_when]\n", file),
               "field 'applies_when' must set all, any or both");
  expect_error(
      parse_manifest(std::string(manifest_head) +
                         "[applies_when]\nany = [{ path_exists = 'x', os_build_min = 1 }]\n",
                     file),
      "applies_when.any #1: must set exactly one of");
  expect_error(
      parse_manifest(
          std::string(manifest_head) + "[applies_when]\nany = [{ app_installed = '(' }]\n", file),
      "field 'app_installed' is not a valid regular expression");
  expect_error(parse_manifest(std::string(manifest_head) +
                                  "[applies_when]\nany = [{ path_exists = 'tool\\cache' }]\n",
                              file),
               "field 'path_exists' must hold absolute paths");
  expect_error(parse_manifest(std::string(manifest_head) + "[applies_when]\nany = []\n", file),
               "field 'applies_when.any' must be a non-empty list of conditions");
}

constexpr std::string_view wsl_recipe = R"([[relocate]]
id          = "distro"
label       = "WSL distribution disk"
locations   = ['%LOCALAPPDATA%\Packages\*\LocalState\ext4.vhdx']
current     = { registry = 'HKCU\Software\Microsoft\Windows\CurrentVersion\Lxss\*\BasePath', name = "DistributionName" }
target_kind = "command"
command     = "wsl --manage {name} --move {target}"
content     = "move"
needs       = ["wslhost.exe"]
skip_names  = ["docker-desktop"]
undo        = "command"
)";

TEST(Relocate, ParsesEnvAndCommandRecipes) {
  const Result<std::vector<RelocationRecipe>> recipes =
      parse_relocate_file(std::string(wsl_recipe) + R"(
[[relocate]]
id          = "home"
label       = "Tool home"
locations   = ['%USERPROFILE%\.tool']
current     = { env = "TOOL_HOME" }
target_kind = "env"
content     = "regenerate"
undo        = "restore"

[[relocate]]
id          = "cache"
label       = "Tool cache setting"
locations   = ['%LOCALAPPDATA%\tool-cache']
current     = { config = '%USERPROFILE%\.toolrc', key = "cache", format = "ini" }
target_kind = "config"
content     = "regenerate"
undo        = "none"
note        = "Remove the cache line from .toolrc to go back."
)",
                          file);
  ASSERT_TRUE(recipes.has_value()) << recipes.error().message;
  ASSERT_EQ(recipes->size(), 3U);
  const RelocationRecipe& wsl = (*recipes)[0];
  EXPECT_EQ(wsl.current.kind, SettingKind::registry);
  EXPECT_EQ(wsl.current.name, "DistributionName");
  EXPECT_EQ(wsl.target_kind, SettingKind::command);
  EXPECT_EQ(wsl.undo, UndoKind::command);
  EXPECT_EQ(wsl.skip_names, (std::vector<std::string>{"docker-desktop"}));
  EXPECT_EQ((*recipes)[1].current.text, "TOOL_HOME");
  EXPECT_EQ((*recipes)[1].content, ContentMode::regenerate);
  EXPECT_EQ((*recipes)[2].current.key, "cache");
  EXPECT_EQ((*recipes)[2].current.format, "ini");
}

// `wsl_recipe` with the text `from` replaced by `to`.
std::string wsl_with(std::string_view from, std::string_view to) {
  std::string text(wsl_recipe);
  text.replace(text.find(from), from.size(), to);
  return text;
}

TEST(Relocate, RejectsInconsistentRecipes) {
  expect_error(parse_relocate_file(wsl_with("{name} --move {target}", "{name} --move later"), file),
               "relocate #1 'distro': field 'command' must hold the placeholder {target}");
  expect_error(parse_relocate_file(wsl_with("{name}", "{distro}"), file),
               "field 'command' may only hold the placeholders {target} and {name}");
  expect_error(parse_relocate_file(wsl_with(R"(, name = "DistributionName")", ""), file),
               "field 'command' may only hold the placeholder {target}");
  expect_error(
      parse_relocate_file(wsl_with(R"(undo        = "command")", R"(undo = "restore")"), file),
      "field 'undo' must be command or none for target_kind 'command'");
  expect_error(
      parse_relocate_file(wsl_with(R"(undo        = "command")", R"(undo = "none")"), file),
      "field 'note' is required with undo 'none'");
  expect_error(
      parse_relocate_file(wsl_with(R"(target_kind = "command")", R"(target_kind = "env")"), file),
      "field 'command' is only allowed with target_kind 'command'");
  expect_error(parse_relocate_file(wsl_with("HKCU\\", "HKCR\\"), file),
               "field 'current.registry' must start with HKCU\\ or HKLM\\");
  expect_error(parse_relocate_file(wsl_with(R"(name = "DistributionName" })",
                                            R"(name = "DistributionName", env = "X" })"),
                                   file),
               "field 'current' must set exactly one of env, config, registry and command");
  expect_error(
      parse_relocate_file(
          wsl_with(R"(needs       = ["wslhost.exe"])", R"(needs = ['C:\wsl\wslhost.exe'])"), file),
      "field 'needs' must hold names without a folder");
  expect_error(
      parse_relocate_file(wsl_with(R"(content     = "move")", R"(content = "copy")"), file),
      "field 'content' must be one of move, regenerate, not 'copy'");
  expect_error(parse_relocate_file(wsl_with(R"(['%LOCALAPPDATA%\Packages\*\LocalState\ext4.vhdx'])",
                                            R"(['LocalState\ext4.vhdx'])"),
                                   file),
               "field 'locations' must hold absolute paths");
  expect_error(parse_relocate_file(std::string(wsl_recipe) + std::string(wsl_recipe), file),
               "relocate #2 'distro': field 'id' repeats an id used earlier in the file");
  expect_error(parse_relocate_file("", file), "field 'relocate' must be a non-empty array");
}

TEST(Relocate, AnAutomaticRecipeNamesWhatMustNotRunAndHasAnUndo) {
  const Result<std::vector<RelocationRecipe>> automatic = parse_relocate_file(
      wsl_with(R"(undo        = "command")", "undo        = \"command\"\nautomatic   = true"),
      file);
  ASSERT_TRUE(automatic.has_value()) << automatic.error().message;
  EXPECT_TRUE(automatic->front().automatic);
  EXPECT_FALSE(parse_relocate_file(std::string(wsl_recipe), file).value().front().automatic);
  expect_error(
      parse_relocate_file(wsl_with(R"(needs       = ["wslhost.exe"])", "automatic = true"), file),
      "field 'automatic' needs `needs` (what must not run) and an undo");
}

// `wsl_recipe` as the WSL 1 recipe: target_kind wsl-import, and `where` as its instance filter.
std::string wsl1_with(std::string_view where) {
  std::string text =
      wsl_with("target_kind = \"command\"\ncommand     = \"wsl --manage {name} --move {target}\"",
               "target_kind = \"wsl-import\"");
  constexpr std::string_view name = R"(name = "DistributionName" })";
  text.replace(text.find(name), name.size(),
               "name = \"DistributionName\", where = " + std::string(where) + " }");
  return text;
}

TEST(Relocate, ParsesAnInstanceFilterAndTheWslImportKind) {
  const Result<std::vector<RelocationRecipe>> recipes =
      parse_relocate_file(wsl1_with(R"({ value = "Flags", mask = 8, equals = 0 })"), file);
  ASSERT_TRUE(recipes.has_value()) << recipes.error().message;
  const RelocationRecipe& recipe = recipes->at(0);
  EXPECT_EQ(recipe.target_kind, SettingKind::wsl_import);
  EXPECT_EQ(recipe.undo, UndoKind::command);
  ASSERT_TRUE(recipe.current.where.has_value());
  const InstanceFilter filter = recipe.current.where.value_or(InstanceFilter{});
  EXPECT_EQ(filter.value, "Flags");
  EXPECT_EQ(filter.mask, 8U);
  EXPECT_EQ(filter.equals, 0U);
  EXPECT_EQ(parse_setting_kind("wsl-import"), SettingKind::wsl_import);
  EXPECT_EQ(parse_undo_kind("restore"), UndoKind::restore);
  EXPECT_EQ(parse_content_mode("copy"), std::nullopt);
}

TEST(Relocate, RejectsBadFiltersAndWslImportMisuse) {
  expect_error(parse_relocate_file(wsl1_with(R"({ value = "Flags", mask = 8, equals = 9 })"), file),
               "field 'current.where.equals' has bits that mask leaves out");
  expect_error(parse_relocate_file(wsl1_with(R"({ value = "Flags", equals = 0 })"), file),
               "field 'current.where.mask' is missing");
  expect_error(
      parse_relocate_file(wsl_with(R"(, name = "DistributionName" })",
                                   R"(, where = { value = "Flags", mask = 8, equals = 8 } })"),
                          file),
      "field 'current.where' needs instances");
  std::string restoring = wsl1_with(R"({ value = "Flags", mask = 8, equals = 0 })");
  restoring.replace(restoring.find(R"(undo        = "command")"), 23, R"(undo = "restore")");
  expect_error(parse_relocate_file(restoring, file),
               "field 'undo' must be command or none for target_kind 'wsl-import'");
  std::string nameless =
      wsl_with("target_kind = \"command\"\ncommand     = \"wsl --manage {name} --move {target}\"",
               "target_kind = \"wsl-import\"");
  nameless.replace(nameless.find(R"(, name = "DistributionName")"), 27, "");
  expect_error(parse_relocate_file(nameless, file),
               "field 'target_kind' 'wsl-import' needs instances");
  expect_error(
      parse_relocate_file(wsl_with("{ registry = ", "{ wsl-import = 'x', registry = "), file),
      "field 'current.wsl-import' is not a known field");
}

TEST(Relocate, RejectsMismatchedSettingKinds) {
  constexpr std::string_view env = R"([[relocate]]
id          = "home"
label       = "Tool home"
locations   = ['%USERPROFILE%\.tool']
current     = { env = "TOOL_HOME" }
target_kind = "registry"
content     = "move"
undo        = "restore"
)";
  expect_error(parse_relocate_file(env, file),
               "field 'target_kind' must be 'env': it writes the setting `current` names");
  std::string config(env);
  config.replace(config.find(R"({ env = "TOOL_HOME" })"), 21,
                 R"({ config = '%USERPROFILE%\.toolrc', key = "cache" })");
  expect_error(parse_relocate_file(config, file), "field 'current.format' is missing");
  std::string keyed(env);
  keyed.replace(keyed.find(R"({ env = "TOOL_HOME" })"), 21, R"({ env = "TOOL_HOME", key = "k" })");
  expect_error(parse_relocate_file(keyed, file), "field 'current.key' is only allowed with config");
  std::string skipping(env);
  skipping.replace(skipping.find("target_kind = \"registry\""), 24,
                   "target_kind = \"env\"\nskip_names = [\"x\"]");
  expect_error(parse_relocate_file(skipping, file), "field 'skip_names' needs instances");
}

TEST(Compress, ParsesTargetsAndRejectsBadOnes) {
  constexpr std::string_view target = R"([[compress]]
id        = "toolchains"
label     = "Toolchains"
paths     = ['%USERPROFILE%\.tool\toolchains']
algorithm = "lzx"
exclude   = ["*.pdb", "cache"]
)";
  const Result<std::vector<CompressTarget>> targets = parse_compress_file(target, file);
  ASSERT_TRUE(targets.has_value()) << targets.error().message;
  EXPECT_EQ(targets->at(0).algorithm, "lzx");
  EXPECT_EQ(targets->at(0).exclude.size(), 2U);

  std::string lznt1(target);
  lznt1.replace(lznt1.find("\"lzx\""), 5, "\"lznt1\"");
  expect_error(parse_compress_file(lznt1, file),
               "field 'algorithm' must be one of xpress4k, xpress8k, xpress16k, lzx");
  std::string folder(target);
  folder.replace(folder.find("\"cache\""), 7, R"('cache\x')");
  expect_error(parse_compress_file(folder, file),
               "field 'exclude' must hold names without a folder");
  std::string any_drive(target);
  any_drive.replace(any_drive.find("%USERPROFILE%"), 13, "?:");
  expect_error(parse_compress_file(any_drive, file), "field 'paths' must name a drive");
}

}  // namespace
}  // namespace myclean::packs
