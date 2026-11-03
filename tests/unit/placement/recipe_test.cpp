#include "core/placement/recipe.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "../cleanup/fakes.hpp"
#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/packs/schema.hpp"
#include "core/placement/settings.hpp"
#include "fakes/fake_file_system.hpp"
#include "placement_fakes.hpp"

namespace myclean::placement {
namespace {

using namespace literals;
using packs::RelocationRecipe;
using packs::SettingKind;
using testing::FakeDriveFacts;
using testing::FakeProcessTable;
using testing::FakeRegistry;
using testing::FakeTextFiles;
using testing::FakeTreeCopier;
using testing::FakeUserEnvironment;

constexpr std::string_view lxss = R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Lxss)";
constexpr std::string_view ubuntu_folder =
    R"(C:\Users\owner\AppData\Local\Packages\CanonicalGroupLimited.Ubuntu24.04LTS_79rhkp1fndgsc\LocalState)";
constexpr std::string_view wsl1_target = R"(D:\Relocated\wsl-docker\distro-files\Ubuntu-24.04)";

RelocationRecipe env_recipe() {
  return {.id = "ai-tools/ollama-models",
          .label = "Ollama models (OLLAMA_MODELS)",
          .locations = {R"(%USERPROFILE%\.ollama\models)"},
          .current = {.kind = SettingKind::env, .text = "OLLAMA_MODELS"},
          .target_kind = SettingKind::env,
          .content = packs::ContentMode::move,
          .needs = {"ollama.exe"},
          .undo = packs::UndoKind::restore};
}

RelocationRecipe config_recipe(std::string format, std::string file, std::string key) {
  return {.id = "tool/cache",
          .label = "Tool cache",
          .locations = {R"(%LOCALAPPDATA%\tool-cache)"},
          .current = {.kind = SettingKind::config,
                      .text = std::move(file),
                      .key = std::move(key),
                      .format = std::move(format)},
          .target_kind = SettingKind::config,
          .content = packs::ContentMode::regenerate,
          .undo = packs::UndoKind::restore};
}

RelocationRecipe wsl_recipe(SettingKind target_kind) {
  const bool wsl2 = target_kind == SettingKind::command;
  return {
      .id = wsl2 ? "wsl-docker/distro" : "wsl-docker/distro-files",
      .label = wsl2 ? "WSL distribution disk (ext4.vhdx)" : "WSL 1 distribution file system",
      .locations = {R"(%LOCALAPPDATA%\Packages\*\LocalState\ext4.vhdx)"},
      .current = {.kind = SettingKind::registry,
                  .text = std::string(lxss) + R"(\*\BasePath)",
                  .name = "DistributionName",
                  .where =
                      packs::InstanceFilter{.value = "Flags", .mask = 8, .equals = wsl2 ? 8U : 0U}},
      .target_kind = target_kind,
      .command = wsl2 ? "wsl --manage {name} --move {target}" : "",
      .content = packs::ContentMode::move,
      .needs = {"wslhost.exe"},
      .skip_names = {"docker-desktop"},
      .undo = packs::UndoKind::command};
}

std::vector<std::string> words(const std::string& text) {
  std::istringstream stream(text);
  std::vector<std::string> result;
  for (std::string word; stream >> word;) {
    result.push_back(word);
  }
  return result;
}

// Previews and applies.
Result<Undo> relocate(RecipeAction& action) {
  const Result<Preview> preview = action.preview();
  if (!preview) {
    return tl::unexpected(preview.error());
  }
  return action.apply(*preview);
}

class RecipeTest : public ::testing::Test {
 protected:
  RecipeTest() {
    drives_.list() = testing::owner_volumes();
    for (const TargetVolume& volume : drives_.list()) {
      fs_.add_directory(volume.root);
    }
    fs_.add_directory(LR"(C:\Users\owner)");
    // wsl.exe as far as these tests need it: the registry and files change as the real one
    // changes them.
    runner_.on_run = [this](const std::string& call) { simulate(words(call)); };
  }

  void add_distro(const std::string& guid, const std::string& name, std::string_view folder,
                  std::uint32_t flags, const std::string& uid = "1000") {
    const std::string key = std::string(lxss) + "\\" + guid;
    registry_.put(key, "DistributionName", RegistryValue{.text = name});
    registry_.put(key, "BasePath", RegistryValue{.text = std::string(folder)});
    registry_.put(key, "Flags",
                  RegistryValue{.text = std::to_string(flags), .type = RegistryType::dword});
    registry_.put(key, "DefaultUid", RegistryValue{.text = uid, .type = RegistryType::dword});
  }

  // The key of the distribution named `name`; empty when none is registered.
  std::string key_of(std::string_view name) {
    const std::vector<std::string> subkeys = registry_.subkeys(lxss).value();
    for (const std::string& subkey : subkeys) {
      const std::string key = std::string(lxss) + "\\" + subkey;
      const std::optional<RegistryValue> value = registry_.read(key, "DistributionName").value();
      if (value && value->text == name) {
        return key;
      }
    }
    return {};
  }

  std::string value(const std::string& key, std::string_view name) {
    const std::optional<RegistryValue> found = registry_.read(key, name).value();
    return found ? found->text : "<absent>";
  }

  RecipeAction action(RecipeRelocation relocation) {
    return RecipeAction{tools_, runner_, std::move(relocation), {}};
  }

  void answer(const std::string& call, std::uint32_t code = 0) {
    runner_.answer(call, "", code, code == 0 ? "" : "Error: injected");
  }

  FakeUserEnvironment& env() { return env_; }
  FakeTextFiles& files() { return files_; }
  fakes::FakeFileSystem& fs() { return fs_; }
  FakeTreeCopier& copier() { return copier_; }
  FakeRegistry& registry() { return registry_; }
  FakeProcessTable& processes() { return processes_; }
  cleanup::testing::FakeRunner& runner() { return runner_; }
  [[nodiscard]] const Tools& tools() const { return tools_; }

 private:
  void simulate(const std::vector<std::string>& call) {
    std::string joined;
    for (const std::string& word : call) {
      joined += (joined.empty() ? "" : " ") + word;
    }
    const auto found = runner_.answers.find(joined);
    if (call.size() < 3 || found == runner_.answers.end() || !found->second ||
        found->second->exit_code != 0) {
      return;  // a failing command changes nothing
    }
    if (call[1] == "--export") {
      fs_.add_file(testing::path_of(call[3]), 2_GiB);
    } else if (call[1] == "--import") {
      add_distro("{imported-" + std::to_string(++imports_) + "}", call[2], call[3], 7, "0");
      fs_.add_file(testing::path_of(call[3]) / "rootfs" / "etc" / "passwd", 2_GiB);
    } else if (call[1] == "--unregister") {
      registry_.drop(key_of(call[2]));
    } else if (call[1] == "--manage" && call.size() == 5) {
      registry_.put(key_of(call[2]), "BasePath", RegistryValue{.text = call[4]});
    }
  }

  FakeUserEnvironment env_;
  FakeTextFiles files_;
  fakes::FakeFileSystem fs_;
  FakeTreeCopier copier_{fs_};
  FakeDriveFacts drives_;
  FakeRegistry registry_;
  FakeProcessTable processes_;
  cleanup::testing::FakeRunner runner_;
  int imports_ = 0;
  Tools tools_{.environment = &env_,
               .files = &files_,
               .copier = &copier_,
               .file_system = &fs_,
               .drives = &drives_,
               .registry = &registry_,
               .processes = &processes_};
};

TEST_F(RecipeTest, AnEnvRecipeCopiesTheContentThenSetsTheVariableAndUndoesExactly) {
  fs().add_file(LR"(C:\Users\owner\.ollama\models\blobs\a)", 4_GiB);
  RecipeAction move = action({.recipe = env_recipe(),
                              .target = LR"(D:\Relocated\ai-tools\ollama-models)",
                              .content = ContentMode::move});

  const Result<Preview> preview = move.preview();
  ASSERT_TRUE(preview.has_value()) << preview.error().message;
  ASSERT_EQ(preview->items.size(), 1U);
  EXPECT_EQ(preview->items[0].path, LR"(C:\Users\owner\.ollama\models)");
  EXPECT_EQ(preview->items[0].size, 4_GiB);
  EXPECT_EQ(env().writes(), 0);

  const Result<Undo> undo = move.apply(*preview);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  EXPECT_TRUE(undo->reversible());
  EXPECT_TRUE(fs().exists(LR"(D:\Relocated\ai-tools\ollama-models\blobs\a)"));
  EXPECT_EQ(env().value("OLLAMA_MODELS"),
            (EnvValue{.text = R"(D:\Relocated\ai-tools\ollama-models)", .expandable = false}));
  EXPECT_EQ(env().announcements(), 1);
  const Result<Verification> verification = move.verify(*undo);
  ASSERT_TRUE(verification.has_value()) << verification.error().message;
  EXPECT_EQ(verification->effect, Effect::better);

  // Rebuilt from the task after a restart, as the tray does for an undo.
  RecipeAction back = action({.recipe = env_recipe()});
  ASSERT_TRUE(back.undo(*undo).has_value());
  EXPECT_EQ(env().value("OLLAMA_MODELS"), std::nullopt);  // it was not set before
  EXPECT_EQ(env().announcements(), 2);
}

TEST_F(RecipeTest, UndoRefusesAndChangesNothingWhenTheSettingWasChangedMeanwhile) {
  RecipeAction move = action({.recipe = env_recipe(),
                              .target = LR"(D:\Relocated\ai-tools\ollama-models)",
                              .content = ContentMode::regenerate});
  const Result<Undo> undo = relocate(move);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  env().put("OLLAMA_MODELS", R"(E:\models)");

  const Result<void> undone = move.undo(*undo);
  ASSERT_FALSE(undone.has_value());
  EXPECT_EQ(undone.error().code, Errc::invalid_state);
  EXPECT_EQ(env().value("OLLAMA_MODELS"), (EnvValue{.text = R"(E:\models)"}));
  const Result<Verification> verification = move.verify(*undo);
  ASSERT_TRUE(verification.has_value());
  EXPECT_EQ(verification->effect, Effect::worse);
}

TEST_F(RecipeTest, NothingMovesWhileAProgramItNeedsRuns) {
  fs().add_file(LR"(C:\Users\owner\.ollama\models\blobs\a)", 1_GiB);
  processes().images = {"explorer.exe", "Ollama.exe"};
  RecipeAction move = action({.recipe = env_recipe(),
                              .target = LR"(D:\Relocated\ai-tools\ollama-models)",
                              .content = ContentMode::move});

  const Result<Undo> undo = relocate(move);
  ASSERT_FALSE(undo.has_value());
  EXPECT_EQ(undo.error().code, Errc::in_use);
  EXPECT_EQ(copier().copies(), 0);
  EXPECT_EQ(env().writes(), 0);
}

TEST_F(RecipeTest, AnIniConfigKeyIsWrittenAndTheFileMyCleanCreatedGoesAwayOnUndo) {
  const std::filesystem::path file = LR"(C:\Users\owner\.toolrc)";
  RecipeAction move = action({.recipe = config_recipe("ini", R"(%USERPROFILE%\.toolrc)", "cache"),
                              .target = LR"(D:\Relocated\tool\cache)",
                              .content = ContentMode::regenerate});

  const Result<Preview> preview = move.preview();
  ASSERT_TRUE(preview.has_value()) << preview.error().message;
  EXPECT_EQ(preview->items[0].path, LR"(C:\Users\owner\AppData\Local\tool-cache)");  // default
  const Result<Undo> undo = move.apply(*preview);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  EXPECT_EQ(files().read(file).value(),
            std::optional<std::string>{"cache=D:\\Relocated\\tool\\cache\n"});
  EXPECT_TRUE(fs().exists(LR"(D:\Relocated\tool\cache)"));
  EXPECT_EQ(move.verify(*undo).value().effect, Effect::better);

  ASSERT_TRUE(move.undo(*undo).has_value());
  EXPECT_FALSE(files().exists(file));
}

TEST_F(RecipeTest, AJsonConfigKeyChangesAloneAndComesBack) {
  const std::filesystem::path file = LR"(C:\Users\owner\.tool\settings.json)";
  ASSERT_TRUE(
      files()
          .write(file, R"({"theme": "dark", "cachePath": "C:\\Users\\owner\\.tool\\cache"})")
          .has_value());
  RecipeAction move =
      action({.recipe = config_recipe("json", R"(%USERPROFILE%\.tool\settings.json)", "cachePath"),
              .target = LR"(D:\Relocated\tool\cache)",
              .content = ContentMode::regenerate});

  const Result<Preview> preview = move.preview();
  ASSERT_TRUE(preview.has_value()) << preview.error().message;
  EXPECT_EQ(preview->items[0].path, LR"(C:\Users\owner\.tool\cache)");
  const Result<Undo> undo = move.apply(*preview);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  const nlohmann::json written = nlohmann::json::parse(*files().read(file).value());
  EXPECT_EQ(written,
            (nlohmann::json{{"theme", "dark"}, {"cachePath", R"(D:\Relocated\tool\cache)"}}));

  ASSERT_TRUE(move.undo(*undo).has_value());
  EXPECT_EQ(nlohmann::json::parse(*files().read(file).value()),
            (nlohmann::json{{"theme", "dark"}, {"cachePath", R"(C:\Users\owner\.tool\cache)"}}));
}

TEST_F(RecipeTest, ARegistryValueKeepsItsTypeAndIsRestoredExactly) {
  constexpr std::string_view key = R"(HKCU\Software\Tool)";
  registry().put(key, "CachePath",
                 RegistryValue{.text = R"(%LOCALAPPDATA%\tool)", .type = RegistryType::expandable});
  RecipeAction move = action({.recipe = {.id = "tool/registry-cache",
                                         .label = "Tool cache",
                                         .locations = {R"(%LOCALAPPDATA%\tool)"},
                                         .current = {.kind = SettingKind::registry,
                                                     .text = std::string(key) + R"(\CachePath)"},
                                         .target_kind = SettingKind::registry,
                                         .content = packs::ContentMode::regenerate,
                                         .undo = packs::UndoKind::restore},
                              .target = LR"(D:\Relocated\tool\registry-cache)",
                              .content = ContentMode::regenerate});

  const Result<Undo> undo = relocate(move);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  EXPECT_EQ(registry().read(key, "CachePath").value(),
            (RegistryValue{.text = R"(D:\Relocated\tool\registry-cache)",
                           .type = RegistryType::expandable}));
  ASSERT_TRUE(move.undo(*undo).has_value());
  EXPECT_EQ(registry().read(key, "CachePath").value(),
            (RegistryValue{.text = R"(%LOCALAPPDATA%\tool)", .type = RegistryType::expandable}));
}

TEST_F(RecipeTest, AWsl2DistributionMovesWithItsOwnCommandUnderItsRegisteredName) {
  add_distro("{a}", "Ubuntu-24.04", R"(\\?\C:\Users\owner\AppData\Local\wsl\{a})", 15);
  fs().add_file(LR"(C:\Users\owner\AppData\Local\wsl\{a}\ext4.vhdx)", 8_GiB);
  const std::string there =
      R"(wsl --manage Ubuntu-24.04 --move D:\Relocated\wsl-docker\distro\Ubuntu-24.04)";
  const std::string back =
      R"(wsl --manage Ubuntu-24.04 --move C:\Users\owner\AppData\Local\wsl\{a})";
  answer(there);
  answer(back);
  RecipeAction move = action({.recipe = wsl_recipe(SettingKind::command),
                              .instance = "ubuntu-24.04",  // any case
                              .target = LR"(D:\Relocated\wsl-docker\distro\Ubuntu-24.04)"});

  const Result<Preview> preview = move.preview();
  ASSERT_TRUE(preview.has_value()) << preview.error().message;
  EXPECT_EQ(preview->items[0].path, LR"(C:\Users\owner\AppData\Local\wsl\{a})");
  EXPECT_EQ(preview->items[0].size, 8_GiB);
  const Result<Undo> undo = move.apply(*preview);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  EXPECT_EQ(runner().calls, std::vector<std::string>{there});
  EXPECT_EQ(move.verify(*undo).value().effect, Effect::better);

  ASSERT_TRUE(move.undo(*undo).has_value());
  EXPECT_EQ(runner().calls, (std::vector<std::string>{there, back}));
  EXPECT_EQ(value(key_of("Ubuntu-24.04"), "BasePath"), R"(C:\Users\owner\AppData\Local\wsl\{a})");
}

TEST_F(RecipeTest, OnlyTheInstancesTheRecipeKeepsAndDoesNotSkipAreMoved) {
  add_distro("{a}", "Debian", R"(C:\Users\owner\AppData\Local\Packages\Debian\LocalState)", 7);
  add_distro("{b}", "docker-desktop", R"(C:\Users\owner\AppData\Local\Docker\wsl\main)", 15);
  RecipeAction wsl1 = action({.recipe = wsl_recipe(SettingKind::command),
                              .instance = "Debian",
                              .target = LR"(D:\Relocated\wsl-docker\distro\Debian)"});
  EXPECT_EQ(wsl1.preview().error().code, Errc::not_found);  // WSL 1: not for --manage --move
  RecipeAction docker = action({.recipe = wsl_recipe(SettingKind::command),
                                .instance = "Docker-Desktop",
                                .target = LR"(D:\Relocated\wsl-docker\distro\docker-desktop)"});
  EXPECT_EQ(docker.preview().error().code, Errc::refused);
  RecipeAction unknown = action({.recipe = wsl_recipe(SettingKind::command),
                                 .instance = "Fedora",
                                 .target = LR"(D:\Relocated\wsl-docker\distro\Fedora)"});
  EXPECT_EQ(unknown.preview().error().code, Errc::not_found);
  EXPECT_TRUE(runner().calls.empty());
}

TEST_F(RecipeTest, LocationsAreEveryKeptInstanceOrTheOnePlaceOfARecipe) {
  add_distro("{a}", "Debian", R"(C:\Users\owner\AppData\Local\Packages\Debian\LocalState)", 7);
  add_distro("{b}", "docker-desktop", R"(C:\Users\owner\AppData\Local\Docker\wsl\main)", 15);
  add_distro("{c}", "Ubuntu", R"(C:\Users\owner\AppData\Local\wsl\{c})", 15);

  const Result<std::vector<RecipeLocation>> distros =
      recipe_locations(tools(), runner(), wsl_recipe(SettingKind::command));
  ASSERT_TRUE(distros.has_value()) << distros.error().message;
  ASSERT_EQ(distros->size(), 1U);  // Debian is WSL 1, docker-desktop is skipped
  EXPECT_EQ(distros->front().instance, "Ubuntu");
  EXPECT_EQ(distros->front().path,
            std::filesystem::path(LR"(C:\Users\owner\AppData\Local\wsl\{c})"));

  // Not set: the recipe's default location.
  const Result<std::vector<RecipeLocation>> models =
      recipe_locations(tools(), runner(), env_recipe());
  ASSERT_TRUE(models.has_value()) << models.error().message;
  ASSERT_EQ(models->size(), 1U);
  EXPECT_TRUE(models->front().instance.empty());
  EXPECT_EQ(models->front().path, std::filesystem::path(LR"(C:\Users\owner\.ollama\models)"));
  EXPECT_TRUE(runner().calls.empty());
}

TEST_F(RecipeTest, IdleOnlyWhileNoProgramItNeedsRuns) {
  processes().images = {"explorer.exe"};
  EXPECT_TRUE(recipe_idle(tools(), env_recipe()).has_value());
  processes().images = {"explorer.exe", "Ollama.exe"};
  EXPECT_EQ(recipe_idle(tools(), env_recipe()).error().code, Errc::in_use);
}

TEST_F(RecipeTest, UndoOfACommandMoveRefusesWhenTheDistributionMovedMeanwhile) {
  add_distro("{a}", "Ubuntu", R"(C:\Users\owner\AppData\Local\wsl\{a})", 15);
  const std::string there = R"(wsl --manage Ubuntu --move D:\Relocated\wsl-docker\distro\Ubuntu)";
  answer(there);
  RecipeAction move = action({.recipe = wsl_recipe(SettingKind::command),
                              .instance = "Ubuntu",
                              .target = LR"(D:\Relocated\wsl-docker\distro\Ubuntu)"});
  const Result<Undo> undo = relocate(move);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  registry().put(key_of("Ubuntu"), "BasePath", RegistryValue{.text = R"(F:\wsl\Ubuntu)"});

  const Result<void> undone = move.undo(*undo);
  ASSERT_FALSE(undone.has_value());
  EXPECT_EQ(undone.error().code, Errc::invalid_state);
  EXPECT_EQ(runner().calls, std::vector<std::string>{there});
}

class Wsl1Test : public RecipeTest {
 protected:
  Wsl1Test() {
    add_distro("{c}", "Ubuntu-24.04", ubuntu_folder, 7, "1000");
    fs().add_file(testing::path_of(ubuntu_folder) / "rootfs" / "etc" / "passwd", 2_GiB);
  }

  RecipeAction wsl1() {
    return action({.recipe = wsl_recipe(SettingKind::wsl_import),
                   .instance = "Ubuntu-24.04",
                   .target = testing::path_of(wsl1_target)});
  }
};

// The wsl.exe calls that move Ubuntu-24.04 to wsl1_target.
struct Wsl1Calls {
  std::string target{wsl1_target};
  std::string archive = target + R"(\Ubuntu-24.04.tar)";
  std::string exported = "wsl --export Ubuntu-24.04 " + archive;
  std::string imported =
      "wsl --import Ubuntu-24.04-myclean-move " + target + " " + archive + " --version 1";
  std::string checked = "wsl -d Ubuntu-24.04-myclean-move -e true";
  std::string unregistered = "wsl --unregister Ubuntu-24.04";
  std::string removed_copy = "wsl --unregister Ubuntu-24.04-myclean-move";
};

TEST_F(Wsl1Test, MovesByExportAndImportAndUnregistersOnlyAfterAVerifiedImport) {
  const Wsl1Calls call;
  for (const std::string& each : {call.exported, call.imported, call.checked, call.unregistered}) {
    answer(each);
  }
  bool copy_checked_first = false;
  const auto simulate = runner().on_run;
  runner().on_run = [&](const std::string& run) {
    if (run == call.unregistered) {
      const std::string copy = key_of("Ubuntu-24.04-myclean-move");
      copy_checked_first = !copy.empty() && value(copy, "BasePath") == call.target &&
                           runner().calls.size() == 4 && runner().calls[2] == call.checked;
    }
    simulate(run);
  };
  RecipeAction move = wsl1();

  const Result<Undo> undo = relocate(move);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  EXPECT_EQ(runner().calls, (std::vector<std::string>{call.exported, call.imported, call.checked,
                                                      call.unregistered}));
  EXPECT_TRUE(copy_checked_first);
  EXPECT_FALSE(registry().has(std::string(lxss) + R"(\{c})"));
  const std::string copy = key_of("Ubuntu-24.04");  // the copy took the original's name
  EXPECT_EQ(copy, std::string(lxss) + R"(\{imported-1})");
  EXPECT_EQ(registry().read(copy, "DefaultUid").value(),
            (RegistryValue{.text = "1000", .type = RegistryType::dword}));
  EXPECT_TRUE(fs().exists(testing::path_of(call.archive)));  // kept as a backup
  EXPECT_EQ(move.verify(*undo).value().effect, Effect::better);
}

TEST_F(Wsl1Test, UndoMovesItBackTheSameWay) {
  const Wsl1Calls call;
  for (const std::string& each : {call.exported, call.imported, call.checked, call.unregistered}) {
    answer(each);
  }
  RecipeAction move = wsl1();
  const Result<Undo> undo = relocate(move);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  const std::string folder{ubuntu_folder};
  const std::string back_archive = folder + R"(\Ubuntu-24.04.tar)";
  const std::vector<std::string> back = {
      "wsl --export Ubuntu-24.04 " + back_archive,
      "wsl --import Ubuntu-24.04-myclean-move " + folder + " " + back_archive + " --version 1",
      call.checked, call.unregistered};
  // The import back lands in a fresh key; the first copy's key is gone with its unregister.
  for (const std::string& each : back) {
    answer(each);
  }
  fs().erase(testing::path_of(ubuntu_folder) / "rootfs");

  ASSERT_TRUE(move.undo(*undo).has_value());
  EXPECT_EQ(std::vector<std::string>(runner().calls.begin() + 4, runner().calls.end()), back);
  const std::string original = key_of("Ubuntu-24.04");
  EXPECT_EQ(value(original, "BasePath"), folder);
  EXPECT_EQ(value(original, "DefaultUid"), "1000");
}

TEST_F(Wsl1Test, AFailedImportLeavesTheOriginalRegistered) {
  const Wsl1Calls call;
  answer(call.exported);
  answer(call.imported, 1);
  RecipeAction move = wsl1();

  const Result<Undo> undo = relocate(move);
  ASSERT_FALSE(undo.has_value());
  EXPECT_NE(undo.error().message.find("stays as it was"), std::string::npos)
      << undo.error().message;
  EXPECT_EQ(runner().calls, (std::vector<std::string>{call.exported, call.imported}));
  EXPECT_EQ(value(std::string(lxss) + R"(\{c})", "DistributionName"), "Ubuntu-24.04");
}

TEST_F(Wsl1Test, AnImportThatDoesNotStartIsRemovedAndTheOriginalKept) {
  const Wsl1Calls call;
  answer(call.exported);
  answer(call.imported);
  answer(call.checked, 1);
  answer(call.removed_copy);
  RecipeAction move = wsl1();

  const Result<Undo> undo = relocate(move);
  ASSERT_FALSE(undo.has_value());
  EXPECT_EQ(runner().calls, (std::vector<std::string>{call.exported, call.imported, call.checked,
                                                      call.removed_copy}));
  EXPECT_EQ(value(std::string(lxss) + R"(\{c})", "DistributionName"), "Ubuntu-24.04");
  EXPECT_TRUE(key_of("Ubuntu-24.04-myclean-move").empty());
}

TEST_F(Wsl1Test, TheTargetMustBeEmptyAndHoldTheArchiveAndTheFiles) {
  const Wsl1Calls call;
  fs().add_file(testing::path_of(call.target) / "old.txt", 1_MiB);
  RecipeAction move = wsl1();
  EXPECT_EQ(relocate(move).error().code, Errc::not_empty);
  EXPECT_TRUE(runner().calls.empty());
}

TEST(RecipeJson, RoundTripsEveryField) {
  RelocationRecipe recipe = wsl_recipe(SettingKind::wsl_import);
  recipe.note = "Stop the distribution first.";
  const Result<RelocationRecipe> back = recipe_from_json(recipe_json(recipe));
  ASSERT_TRUE(back.has_value()) << back.error().message;
  EXPECT_EQ(recipe_json(*back), recipe_json(recipe));
  EXPECT_EQ(back->current.where.value_or(packs::InstanceFilter{.equals = 1}).equals, 0U);
  EXPECT_EQ(back->target_kind, SettingKind::wsl_import);

  nlohmann::json broken = recipe_json(recipe);
  broken["target_kind"] = "junction";
  EXPECT_EQ(recipe_from_json(broken).error().code, Errc::invalid_data);
}

TEST(RecipeCommands, ShowWhatRunsForTheOwnersApproval) {
  EXPECT_EQ(recipe_commands({.recipe = wsl_recipe(SettingKind::command),
                             .instance = "Ubuntu",
                             .target = LR"(D:\Relocated\wsl-docker\distro\Ubuntu)"}),
            std::vector<std::string>{
                R"(wsl --manage Ubuntu --move D:\Relocated\wsl-docker\distro\Ubuntu)"});
  const std::vector<std::string> wsl1 = recipe_commands(
      {.recipe = wsl_recipe(SettingKind::wsl_import), .instance = "Ubuntu", .target = LR"(D:\U)"});
  ASSERT_EQ(wsl1.size(), 5U);
  EXPECT_EQ(wsl1[0], R"(wsl --export Ubuntu D:\U\Ubuntu.tar)");
  EXPECT_EQ(wsl1[3], "wsl --unregister Ubuntu (only after the import is checked)");
}

}  // namespace
}  // namespace myclean::placement
