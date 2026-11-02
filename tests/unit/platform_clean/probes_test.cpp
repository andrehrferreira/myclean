#include "core/clean/probes.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ratio>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <wil/resource.h>
#include <windows.h>

#include "core/clean/patterns.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "platform/clean/win32_probes.hpp"
#include "sandbox.hpp"

namespace myclean::platform {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

const WallTime old_time = std::chrono::sys_days{std::chrono::year{2020} / 1 / 1};

// Sets the last-write time of `path` to `when`, and its creation time too when `creation`.
void set_times(const fs::path& path, WallTime when, bool creation) {
  using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
  const std::int64_t ticks =
      std::chrono::duration_cast<Ticks>(when.time_since_epoch()).count() + 116'444'736'000'000'000;
  const FILETIME time{.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFF),
                      .dwHighDateTime = static_cast<DWORD>(ticks >> 32)};
  const wil::unique_hfile file{::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
  ASSERT_TRUE(file);
  ASSERT_NE(::SetFileTime(file.get(), creation ? &time : nullptr, nullptr, &time), FALSE);
}

TEST(Win32FileTree, ListsEntriesWithSizesIdentitiesAndTimes) {
  const Sandbox sandbox;
  write_file(sandbox.root() / "a.bin", 10000);
  fs::create_directories(sandbox.root() / "sub");
  set_times(sandbox.root() / "a.bin", old_time, true);

  const Win32FileTree tree;
  const Result<std::vector<clean::Entry>> listed = tree.list(sandbox.root());
  ASSERT_TRUE(listed.has_value()) << listed.error().message;
  ASSERT_EQ(listed->size(), 2U);
  const auto file = std::ranges::find(*listed, sandbox.root() / "a.bin", &clean::Entry::path);
  ASSERT_NE(file, listed->end());
  EXPECT_EQ(file->stat.kind, os::EntryKind::file);
  EXPECT_EQ(file->stat.size, Bytes{10000});
  EXPECT_GE(file->stat.size_on_disk, Bytes{10000});
  EXPECT_EQ(std::chrono::floor<std::chrono::seconds>(file->touched), old_time);

  const Result<clean::Entry> stat = tree.stat(sandbox.root() / "a.bin");
  ASSERT_TRUE(stat.has_value()) << stat.error().message;
  EXPECT_EQ(stat->stat.identity, file->stat.identity);
  EXPECT_EQ(stat->touched, file->touched);
}

TEST(Win32FileTree, NewerCreationTimeCountsAsTouched) {
  const Sandbox sandbox;
  const fs::path path = sandbox.root() / "extracted.bin";
  write_file(path, 100);
  set_times(path, old_time, false);  // as an archive extracts it: old write time, new creation

  const Result<clean::Entry> stat = Win32FileTree{}.stat(path);
  ASSERT_TRUE(stat.has_value()) << stat.error().message;
  EXPECT_GT(stat->touched, std::chrono::system_clock::now() - 1h);
  EXPECT_GT(stat->touched, old_time);
}

TEST(Win32FileTree, DescribesALinkAndNeverListsThroughIt) {
  const Sandbox sandbox;
  write_file(sandbox.root() / "target" / "inside.bin", 100);
  if (!make_junction(sandbox.root() / "link", sandbox.root() / "target")) {
    GTEST_SKIP() << "the file system refused the junction";
  }
  const Win32FileTree tree;
  const Result<clean::Entry> link = tree.stat(sandbox.root() / "link");
  ASSERT_TRUE(link.has_value()) << link.error().message;
  EXPECT_TRUE(link->stat.attributes.reparse_point);
  const Result<std::vector<clean::Entry>> listed = tree.list(sandbox.root() / "link");
  ASSERT_FALSE(listed.has_value());
  EXPECT_EQ(listed.error().code, Errc::invalid_argument);
  EXPECT_EQ(tree.stat(sandbox.root() / "missing").error().code, Errc::not_found);
}

TEST(Win32FileTree, FixedVolumesIncludeTheSystemDriveInLetterOrder) {
  const std::vector<fs::path> volumes = Win32FileTree{}.fixed_volumes();
  std::wstring windows(MAX_PATH, L'\0');
  ASSERT_GT(::GetWindowsDirectoryW(windows.data(), MAX_PATH), 0U);
  const fs::path system = std::wstring{windows.front(), L':', L'\\'};
  EXPECT_NE(std::ranges::find(volumes, system), volumes.end());
  EXPECT_TRUE(std::ranges::is_sorted(volumes));
  for (const fs::path& volume : volumes) {
    EXPECT_EQ(volume.native().size(), 3U);
    EXPECT_EQ(::GetDriveTypeW(volume.c_str()), static_cast<UINT>(DRIVE_FIXED));
  }
}

TEST(Win32Usage, FileOpenWithoutDeleteSharingIsLocked) {
  const Sandbox sandbox;
  const fs::path path = sandbox.root() / "open.log";
  write_file(path, 100);
  const Win32Usage usage;
  EXPECT_FALSE(usage.locked(path));
  {
    const wil::unique_hfile held{::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                               OPEN_EXISTING, 0, nullptr)};
    ASSERT_TRUE(held);
    EXPECT_TRUE(usage.locked(path));
  }
  EXPECT_FALSE(usage.locked(path));
  EXPECT_FALSE(usage.locked(sandbox.root() / "missing"));
}

TEST(Win32Usage, ProcessFoldersIncludeThisProcessDirectoryAndImage) {
  const std::vector<fs::path> folders = Win32Usage{}.process_folders();
  std::wstring image(32768, L'\0');
  image.resize(::GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size())));
  auto contains = [&folders](const fs::path& wanted) {
    return std::ranges::any_of(folders, [&wanted](const fs::path& folder) {
      return clean::within(folder, wanted) && clean::within(wanted, folder);
    });
  };
  EXPECT_TRUE(contains(fs::path{image}.parent_path()));
  EXPECT_TRUE(contains(fs::current_path()));
}

TEST(Win32Environment, ExpandsToTheLongForm) {
  const Win32Environment environment;
  const std::optional<std::wstring> temp = environment.variable(L"temp");
  ASSERT_TRUE(temp.has_value());
  const std::wstring value = temp.value_or(std::wstring{});
  EXPECT_EQ(value.find(L'~'), std::wstring::npos);
  EXPECT_TRUE(fs::is_directory(value));
  EXPECT_EQ(environment.variable(L"MYCLEAN_TEST_SURELY_UNSET"), std::nullopt);
}

// An ordinary variable has its one value; only the tool variables are read from tools' files.
TEST(Win32Environment, OrdinaryVariablesHaveTheirOneValue) {
  const Win32Environment environment;
  EXPECT_EQ(environment.values(L"TEMP"),
            std::vector<std::wstring>{environment.variable(L"TEMP").value_or(L"")});
  EXPECT_TRUE(environment.values(L"MYCLEAN_TEST_SURELY_UNSET").empty());
  EXPECT_EQ(tool_variable(L"TEMP", environment), std::nullopt);
}

}  // namespace
}  // namespace myclean::platform
