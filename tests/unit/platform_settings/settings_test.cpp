// Read-only probes of the real settings, and file operations inside the test sandbox only: no
// environment variable, tool configuration, power, page-file or shadow-storage setting changes.

#include "core/placement/settings.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/relocation.hpp"
#include "platform/fs/win32_file_system.hpp"
#include "platform/settings/process_runner.hpp"
#include "platform/settings/seek_penalty.hpp"
#include "platform/settings/system_space.hpp"
#include "platform/settings/tree_copier.hpp"
#include "platform/settings/user_settings.hpp"
#include "sandbox.hpp"
#include "unit/placement/placement_fakes.hpp"

namespace myclean::platform {
namespace {

using namespace literals;
using testing::Sandbox;

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream{path, std::ios::binary} << content;
}

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream file{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::wstring system_drive() {
  std::array<wchar_t, MAX_PATH> windows{};
  EXPECT_GE(::GetSystemWindowsDirectoryW(windows.data(), MAX_PATH), 3U);
  return {windows[0], L':'};
}

TEST(Win32UserEnvironment, ReadsTheOwnersVariablesWithoutExpandingThem) {
  const Win32UserEnvironment environment;
  const Result<std::optional<placement::EnvValue>> temp = environment.get("TEMP");
  ASSERT_TRUE(temp.has_value()) << temp.error().message;
  if (const std::optional<placement::EnvValue>& value = *temp; value) {
    std::cout << "user TEMP = " << value->text << (value->expandable ? " (REG_EXPAND_SZ)\n" : "\n");
    EXPECT_FALSE(value->text.empty());
  }
  const Result<std::optional<placement::EnvValue>> missing =
      environment.get("MYCLEAN_TEST_VARIABLE_THAT_IS_NEVER_SET");
  ASSERT_TRUE(missing.has_value());
  EXPECT_FALSE(missing->has_value());
}

TEST(Win32UserEnvironment, ExpandsInTheProcessEnvironment) {
  const Win32UserEnvironment environment;
  const Result<std::filesystem::path> drive = environment.expand("%SystemDrive%");
  ASSERT_TRUE(drive.has_value());
  EXPECT_EQ(drive->native(), system_drive());
  EXPECT_EQ(environment.expand("no references")->native(), L"no references");
}

TEST(Win32TextFiles, WritesReadsAndRemovesAFileWhole) {
  const Sandbox sandbox;
  Win32TextFiles files;
  const std::filesystem::path npmrc = sandbox.path() / ".npmrc";
  EXPECT_EQ(*files.read(npmrc), std::nullopt);

  ASSERT_TRUE(files.write(npmrc, "registry=x\r\ncache=D:\\npm\r\n").has_value());
  EXPECT_EQ(*files.read(npmrc), std::optional<std::string>{"registry=x\r\ncache=D:\\npm\r\n"});
  ASSERT_TRUE(files.write(npmrc, "registry=x\r\n").has_value());
  EXPECT_EQ(read_file(npmrc), "registry=x\r\n");
  EXPECT_FALSE(std::filesystem::exists(sandbox.path() / ".npmrc.myclean-new"));

  ASSERT_TRUE(files.remove(npmrc).has_value());
  EXPECT_FALSE(std::filesystem::exists(npmrc));
  EXPECT_TRUE(files.remove(npmrc).has_value());
}

TEST(Win32TreeCopier, CopiesEveryFileAndNeverOverwrites) {
  const Sandbox sandbox;
  const std::filesystem::path from = sandbox.path() / "from";
  write_file(from / "a.txt", "alpha");
  write_file(from / "sub" / "deeper" / "b.bin", std::string(70'000, 'b'));
  write_file(from / "sub" / "c.txt", "");
  std::filesystem::create_directories(from / "empty");
  Win32TreeCopier copier;

  const Result<placement::TreeTotals> copied = copier.copy_tree(from, sandbox.path() / "to");

  ASSERT_TRUE(copied.has_value()) << copied.error().message;
  EXPECT_EQ(copied->files, 3U);
  EXPECT_EQ(copied->bytes, Bytes{70'005});
  EXPECT_EQ(read_file(sandbox.path() / "to" / "sub" / "deeper" / "b.bin"),
            std::string(70'000, 'b'));
  EXPECT_TRUE(std::filesystem::is_directory(sandbox.path() / "to" / "empty"));
  EXPECT_EQ(read_file(from / "a.txt"), "alpha");  // the source is untouched

  const Result<placement::TreeTotals> again = copier.copy_tree(from, sandbox.path() / "to");
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error().code, Errc::already_exists);
}

// A relocation with "move" over the real file system and copier, inside the sandbox; the
// settings it writes go to an in-memory environment.
TEST(Win32TreeCopier, MovesACacheEndToEndThroughTheRelocateAction) {
  const Sandbox sandbox;
  const std::filesystem::path cache = sandbox.path() / "gradle";
  write_file(cache / "caches" / "modules-2" / "a.jar", std::string(5000, 'j'));
  write_file(cache / "wrapper" / "dists" / "gradle.zip", std::string(9000, 'z'));
  placement::testing::FakeUserEnvironment environment;
  environment.put("GRADLE_USER_HOME", utf8(cache));
  placement::testing::FakeTextFiles files;
  placement::testing::FakeDriveFacts drives;
  drives.list() = {placement::TargetVolume{.root = system_drive() + L"\\",
                                           .size = 1000_GiB,
                                           .free = 500_GiB,
                                           .drive_class = placement::DriveClass::nvme,
                                           .healthy = true}};
  Win32FileSystem file_system;
  Win32TreeCopier copier;
  const placement::Tools tools{.environment = &environment,
                               .files = &files,
                               .copier = &copier,
                               .file_system = &file_system,
                               .drives = &drives};
  const std::filesystem::path target = sandbox.path() / "moved";
  placement::RelocateAction action{tools,
                                   {.kind = placement::CacheKind::gradle,
                                    .target = target,
                                    .content = placement::ContentMode::move}};

  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview.has_value()) << preview.error().message;
  const Result<Undo> undo = action.apply(*preview);

  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  EXPECT_EQ(read_file(target / "wrapper" / "dists" / "gradle.zip"), std::string(9000, 'z'));
  EXPECT_TRUE(std::filesystem::exists(cache / "caches" / "modules-2" / "a.jar"));
  EXPECT_EQ(environment.value("GRADLE_USER_HOME"), placement::EnvValue{.text = utf8(target)});
}

TEST(Win32ProcessRunner, RunsAReadOnlySystemToolWithoutAWindow) {
  Win32ProcessRunner runner;
  const Result<placement::CommandResult> result =
      runner.run(placement::Command{.tool = "powercfg.exe", .arguments = {"/a"}});
  ASSERT_TRUE(result.has_value()) << result.error().message;
  EXPECT_EQ(result->exit_code, 0U);
  EXPECT_FALSE(result->output.empty());
}

TEST(Win32ProcessRunner, RunsOnlyToolsOfTheSystemFolder) {
  Win32ProcessRunner runner;
  const Result<placement::CommandResult> path =
      runner.run(placement::Command{.tool = R"(..\powercfg.exe)"});
  ASSERT_FALSE(path.has_value());
  EXPECT_EQ(path.error().code, Errc::invalid_argument);
  const Result<placement::CommandResult> missing =
      runner.run(placement::Command{.tool = "myclean-no-such-tool.exe"});
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, Errc::not_found);
}

TEST(Win32SystemSpace, ReadsHibernationFromTheRegistryAndTheFileEntry) {
  const Win32SystemSpace system;
  const Result<placement::HibernationState> state = system.hibernation();
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(state->file, std::filesystem::path{system_drive() + L"\\hiberfil.sys"});
  EXPECT_GT(state->memory, 1_GiB);
  if (state->mode != placement::HibernationMode::off) {
    EXPECT_GT(state->file_size, Bytes{});
  }
  std::cout << "hibernation " << to_string(state->mode) << ", size percent " << state->size_percent
            << ", hiberfil.sys " << state->file_size.count() << " bytes\n";
}

TEST(Win32SystemSpace, ReadsThePageFileConfigurationOverWmi) {
  const Win32SystemSpace system;
  const Result<placement::PageFileState> state = system.page_files();
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_TRUE(!state->config.automatic || state->config.files.empty());
  for (const placement::PageFileUse& use : state->in_use) {
    EXPECT_GT(use.allocated, Bytes{}) << use.path.string();
    std::cout << "page file in use " << use.path.string() << " " << use.allocated.count()
              << " bytes\n";
  }
  std::cout << "automatic " << state->config.automatic << ", " << state->config.files.size()
            << " settings\n";
}

TEST(Win32SystemSpace, ReadsShadowStorageOrNeedsAdministratorRights) {
  const Win32SystemSpace system;
  const Result<std::vector<placement::ShadowStorage>> storages = system.shadow_storage();
  if (!storages) {
    EXPECT_EQ(storages.error().code, Errc::access_denied) << storages.error().message;
    std::cout << "shadow storage: " << storages.error().message << "\n";
    return;
  }
  for (const placement::ShadowStorage& storage : *storages) {
    EXPECT_FALSE(storage.volume.empty());
    std::cout << "shadow storage " << storage.volume << " on " << storage.diff_volume << ": used "
              << storage.used.count() << ", max "
              << (storage.maximum ? std::to_string(storage.maximum->count()) : "unbounded") << "\n";
  }
}

TEST(Win32SeekPenalty, AtLeastOneDiskAnswers) {
  const Win32SeekPenalty seek;
  int answered = 0;
  for (std::uint32_t disk = 0; disk < 32; ++disk) {
    if (const std::optional<bool> penalty = seek.seek_penalty(disk)) {
      ++answered;
      std::cout << "disk " << disk << (*penalty ? " rotates\n" : " has no seek penalty\n");
    }
  }
  EXPECT_GT(answered, 0);
}

}  // namespace
}  // namespace myclean::platform
