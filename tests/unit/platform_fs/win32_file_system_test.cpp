#include "platform/fs/win32_file_system.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/model/file_identity.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "sandbox.hpp"

namespace myclean::platform {
namespace {

using std::filesystem::path;

class Win32FileSystemTest : public ::testing::Test {
 protected:
  Win32FileSystem& fs() { return fs_; }
  [[nodiscard]] const path& root() const { return sandbox_.root(); }

 private:
  Sandbox sandbox_;
  Win32FileSystem fs_;
};

TEST_F(Win32FileSystemTest, StatDescribesFilesAndFolders) {
  write_file(root() / L"a.bin", 5000);
  write_file(root() / L"b.bin", 10);
  const os::FileStat file = fs().stat(root() / L"a.bin").value();
  EXPECT_EQ(file.kind, os::EntryKind::file);
  EXPECT_FALSE(file.attributes.reparse_point);
  EXPECT_FALSE(file.attributes.cloud_placeholder());
  EXPECT_EQ(file.size, Bytes{5000});
  EXPECT_GE(file.size_on_disk, Bytes{5000});
  EXPECT_NE(file.identity.volume_serial, 0U);
  EXPECT_EQ(fs().stat(root() / L"a.bin")->identity, file.identity);
  EXPECT_NE(fs().stat(root() / L"b.bin")->identity, file.identity);

  const os::FileStat folder = fs().stat(root()).value();
  EXPECT_EQ(folder.kind, os::EntryKind::directory);
  EXPECT_EQ(folder.size, Bytes{});

  EXPECT_EQ(fs().stat(root() / L"missing").error().code, Errc::not_found);
  EXPECT_EQ(fs().stat(L"relative\\a.bin").error().code, Errc::invalid_argument);
}

TEST_F(Win32FileSystemTest, EnumerateListsEntriesWithTheIdentityStatReports) {
  write_file(root() / L"dir\\a.txt", 3);
  std::filesystem::create_directories(root() / L"dir\\sub");
  std::vector<os::DirEntry> entries = fs().enumerate(root() / L"dir").value();
  std::ranges::sort(entries, {}, &os::DirEntry::path);
  ASSERT_EQ(entries.size(), 2U);
  EXPECT_EQ(entries[0].path, root() / L"dir\\a.txt");
  EXPECT_EQ(entries[0].stat.kind, os::EntryKind::file);
  EXPECT_EQ(entries[0].stat.size, Bytes{3});
  EXPECT_EQ(entries[1].stat.kind, os::EntryKind::directory);
  for (const os::DirEntry& entry : entries) {
    EXPECT_EQ(entry.stat.identity, fs().stat(entry.path)->identity);
  }
  EXPECT_TRUE(fs().enumerate(root() / L"dir\\sub")->empty());
  EXPECT_EQ(fs().enumerate(root() / L"dir\\a.txt").error().code, Errc::invalid_argument);
}

TEST_F(Win32FileSystemTest, JunctionIsDescribedAndRemovedAsTheLink) {
  write_file(root() / L"store\\pkg\\index.js", 100);
  ASSERT_TRUE(make_junction(root() / L"node_modules\\pkg", root() / L"store\\pkg"));
  const os::FileStat link = fs().stat(root() / L"node_modules\\pkg").value();
  EXPECT_EQ(link.kind, os::EntryKind::directory);
  EXPECT_TRUE(link.attributes.reparse_point);
  EXPECT_NE(link.identity, fs().stat(root() / L"store\\pkg")->identity);
  EXPECT_EQ(fs().enumerate(root() / L"node_modules\\pkg").error().code, Errc::invalid_argument);

  const std::vector<os::DirEntry> modules = fs().enumerate(root() / L"node_modules").value();
  ASSERT_EQ(modules.size(), 1U);
  EXPECT_TRUE(modules[0].stat.attributes.reparse_point);

  ASSERT_TRUE(fs().remove(root() / L"node_modules\\pkg").has_value());
  EXPECT_FALSE(entry_exists(root() / L"node_modules\\pkg"));
  EXPECT_TRUE(entry_exists(root() / L"store\\pkg\\index.js"));
}

TEST_F(Win32FileSystemTest, DirectorySymlinkRemovedAsTheLink) {
  write_file(root() / L"target\\keep.txt", 10);
  const path link = root() / L"link";
  if (::CreateSymbolicLinkW(
          link.c_str(), (root() / L"target").c_str(),
          SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) == FALSE) {
    GTEST_SKIP() << "creating symbolic links needs Developer Mode or elevation";
  }
  EXPECT_TRUE(fs().stat(link)->attributes.reparse_point);
  EXPECT_EQ(fs().enumerate(link).error().code, Errc::invalid_argument);
  ASSERT_TRUE(fs().remove(link).has_value());
  EXPECT_FALSE(entry_exists(link));
  EXPECT_TRUE(entry_exists(root() / L"target\\keep.txt"));
}

TEST_F(Win32FileSystemTest, RemoveRefusesAFolderThatHasEntries) {
  write_file(root() / L"dir\\a.txt", 1);
  EXPECT_EQ(fs().remove(root() / L"dir").error().code, Errc::not_empty);
  ASSERT_TRUE(fs().remove(root() / L"dir\\a.txt").has_value());
  ASSERT_TRUE(fs().remove(root() / L"dir").has_value());
  EXPECT_FALSE(entry_exists(root() / L"dir"));
  EXPECT_EQ(fs().remove(root() / L"dir").error().code, Errc::not_found);
}

TEST_F(Win32FileSystemTest, RemoveWithIdentityRefusesAnotherEntry) {
  const path file = root() / L"a.tmp";
  write_file(file, 10);
  const FileIdentity planned = fs().stat(file).value().identity;
  ASSERT_TRUE(std::filesystem::remove(file));
  ASSERT_TRUE(std::filesystem::create_directory(root() / L"other"));
  ASSERT_TRUE(make_junction(file, root() / L"other"));  // the same name is now a junction
  EXPECT_EQ(fs().remove(file, planned).error().code, Errc::refused);
  EXPECT_TRUE(entry_exists(file));

  const FileIdentity link = fs().stat(file).value().identity;
  ASSERT_TRUE(fs().remove(file, link).has_value());
  EXPECT_FALSE(entry_exists(file));
  EXPECT_TRUE(entry_exists(root() / L"other"));
  EXPECT_EQ(fs().remove(file, link).error().code, Errc::not_found);
}

TEST_F(Win32FileSystemTest, ExpandShortNamesReturnsTheStoredSpelling) {
  const path file = root() / L"Long Folder Name\\File With Spaces.txt";
  write_file(file, 1);
  std::wstring upper = file.native();
  const std::size_t tail = root().native().size();
  std::transform(upper.begin() + static_cast<std::ptrdiff_t>(tail), upper.end(),
                 upper.begin() + static_cast<std::ptrdiff_t>(tail),
                 [](wchar_t character) { return static_cast<wchar_t>(::towupper(character)); });
  EXPECT_EQ(fs().expand_short_names(upper).value(), file);

  std::wstring short_form(32768, L'\0');
  const DWORD length =
      ::GetShortPathNameW(file.c_str(), short_form.data(), static_cast<DWORD>(short_form.size()));
  ASSERT_GT(length, 0U);
  short_form.resize(length);
  EXPECT_EQ(fs().expand_short_names(short_form).value(), file);

  EXPECT_EQ(fs().expand_short_names(root() / L"missing\\x").error().code, Errc::not_found);
  EXPECT_EQ(fs().expand_short_names(root() / L"*.txt").error().code, Errc::invalid_argument);
}

TEST_F(Win32FileSystemTest, OfflineFileIsAPlaceholderThatIsNeverMoved) {
  const path file = root() / L"cloud.bin";
  write_file(file, 100);
  ASSERT_NE(::SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_OFFLINE), FALSE);
  const os::FileStat stat = fs().stat(file).value();
  EXPECT_TRUE(stat.attributes.offline);
  EXPECT_TRUE(stat.attributes.cloud_placeholder());
  EXPECT_EQ(fs().move_file(file, root() / L"moved.bin").error().code, Errc::invalid_argument);
  EXPECT_TRUE(entry_exists(file));
}

TEST_F(Win32FileSystemTest, RenameKeepsIdentityAndNeverReplaces) {
  write_file(root() / L"a.txt", 1);
  write_file(root() / L"b.txt", 1);
  const os::FileStat before = fs().stat(root() / L"a.txt").value();
  EXPECT_EQ(fs().rename(root() / L"a.txt", root() / L"b.txt").error().code, Errc::already_exists);
  ASSERT_TRUE(fs().rename(root() / L"a.txt", root() / L"c.txt").has_value());
  EXPECT_EQ(fs().stat(root() / L"c.txt")->identity, before.identity);
  EXPECT_EQ(fs().rename(root() / L"a.txt", root() / L"d.txt").error().code, Errc::not_found);
}

TEST_F(Win32FileSystemTest, MoveFileMovesOnlyRegularFiles) {
  write_file(root() / L"a.txt", 7);
  std::filesystem::create_directories(root() / L"dir");
  ASSERT_TRUE(make_junction(root() / L"link", root() / L"dir"));
  ASSERT_TRUE(fs().move_file(root() / L"a.txt", root() / L"dir\\a.txt").has_value());
  EXPECT_EQ(fs().stat(root() / L"dir\\a.txt")->size, Bytes{7});
  EXPECT_EQ(fs().move_file(root() / L"dir", root() / L"dir2").error().code, Errc::invalid_argument);
  EXPECT_EQ(fs().move_file(root() / L"link", root() / L"link2").error().code,
            Errc::invalid_argument);
}

TEST_F(Win32FileSystemTest, CreateDirectoriesNeverGoesThroughALink) {
  ASSERT_TRUE(fs().create_directories(root() / L"a\\b\\c").has_value());
  EXPECT_EQ(fs().stat(root() / L"a\\b\\c")->kind, os::EntryKind::directory);
  ASSERT_TRUE(fs().create_directories(root() / L"a\\b").has_value());

  std::filesystem::create_directories(root() / L"target");
  ASSERT_TRUE(make_junction(root() / L"link", root() / L"target"));
  EXPECT_EQ(fs().create_directories(root() / L"link\\x").error().code, Errc::invalid_argument);
  EXPECT_FALSE(entry_exists(root() / L"target\\x"));
}

TEST_F(Win32FileSystemTest, PathsLongerThanMaxPathWork) {
  path deep = root();
  for (int level = 0; level < 12; ++level) {
    deep /= std::wstring(30, static_cast<wchar_t>(L'a' + level));
  }
  ASSERT_GT(deep.native().size(), static_cast<std::size_t>(MAX_PATH));
  ASSERT_TRUE(fs().create_directories(deep).has_value());
  EXPECT_EQ(fs().stat(deep)->kind, os::EntryKind::directory);
  EXPECT_EQ(fs().expand_short_names(deep).value(), deep);
  // Remove the chain here: the sandbox cleanup (std::filesystem) cannot reach this deep.
  for (path folder = deep; folder != root(); folder = folder.parent_path()) {
    ASSERT_TRUE(fs().remove(folder).has_value());
  }
}

}  // namespace
}  // namespace myclean::platform
