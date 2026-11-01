#include "platform/fs/win32_file_system.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <wil/resource.h>
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

TEST_F(Win32FileSystemTest, RemoveDeletesAReadOnlyFile) {
  // Git object files are read-only: a cache folder holding a clone must still be removable.
  const path file = root() / L"pack.idx";
  write_file(file, 10);
  ASSERT_NE(::SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_READONLY), FALSE);
  const FileIdentity identity = fs().stat(file)->identity;
  const Result<void> removed = fs().remove(file, identity);
  EXPECT_TRUE(removed.has_value()) << removed.error().message;
  EXPECT_FALSE(entry_exists(file));
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

// The bytes of `path`.
std::string content_of(const path& file) {
  std::ifstream stream(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

FILETIME filetime(std::uint64_t ticks) {
  return FILETIME{.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFU),
                  .dwHighDateTime = static_cast<DWORD>(ticks >> 32U)};
}

TEST_F(Win32FileSystemTest, CopyFileKeepsContentAttributesAndTimes) {
  const path source = root() / L"src\\data.bin";
  std::filesystem::create_directories(source.parent_path());
  {
    std::ofstream stream(source, std::ios::binary);
    for (int index = 0; index < 300'000; ++index) {
      stream.put(static_cast<char>(index % 251));
    }
  }
  {
    // 2020-01-01 as creation, 2021-01-01 as last write.
    const wil::unique_hfile file{::CreateFileW(source.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr,
                                               OPEN_EXISTING, 0, nullptr)};
    const FILETIME created = filetime(132'223'104'000'000'000ULL);
    const FILETIME written = filetime(132'539'328'000'000'000ULL);
    ASSERT_NE(::SetFileTime(file.get(), &created, nullptr, &written), FALSE);
  }
  ASSERT_NE(::SetFileAttributesW(source.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY),
            FALSE);
  const os::FileStat before = fs().stat(source).value();
  std::filesystem::create_directories(root() / L"dst");
  const path target = root() / L"dst\\data.bin";

  const Result<os::FileStat> copy = fs().copy_file(source, target, before.identity);
  ASSERT_TRUE(copy.has_value()) << copy.error().message;
  EXPECT_NE(copy->identity, before.identity);
  EXPECT_EQ(copy->size, before.size);
  EXPECT_EQ(content_of(target), content_of(source));
  EXPECT_EQ(::GetFileAttributesW(target.c_str()) &
                (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE),
            ::GetFileAttributesW(source.c_str()) &
                (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE));
  WIN32_FILE_ATTRIBUTE_DATA from{};
  WIN32_FILE_ATTRIBUTE_DATA to{};
  ASSERT_NE(::GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &from), FALSE);
  ASSERT_NE(::GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &to), FALSE);
  EXPECT_EQ(::CompareFileTime(&from.ftCreationTime, &to.ftCreationTime), 0);
  EXPECT_EQ(::CompareFileTime(&from.ftLastWriteTime, &to.ftLastWriteTime), 0);
  EXPECT_TRUE(entry_exists(source));  // a copy: the source stays

  // Durable, whether this process may flush the whole volume or only the file; a read-only
  // copy keeps its attribute.
  const std::vector<path> copies = {target};
  const Result<void> flushed = fs().flush(copies);
  EXPECT_TRUE(flushed.has_value()) << flushed.error().message;
  EXPECT_NE(::GetFileAttributesW(target.c_str()) & FILE_ATTRIBUTE_READONLY, 0U);
  for (const path& file : {source, target}) {
    static_cast<void>(::SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL));
  }
}

TEST_F(Win32FileSystemTest, CopyFileCopiesOnlyTheExpectedRegularLocalFile) {
  write_file(root() / L"a.bin", 10);
  write_file(root() / L"taken.bin", 10);
  std::filesystem::create_directories(root() / L"dir");
  ASSERT_TRUE(make_junction(root() / L"link", root() / L"dir"));
  write_file(root() / L"cloud.bin", 10);
  ASSERT_NE(::SetFileAttributesW((root() / L"cloud.bin").c_str(), FILE_ATTRIBUTE_OFFLINE), FALSE);
  const auto identity = [&](const wchar_t* name) { return fs().stat(root() / name)->identity; };

  EXPECT_EQ(
      fs().copy_file(root() / L"a.bin", root() / L"b.bin", identity(L"taken.bin")).error().code,
      Errc::refused);
  EXPECT_EQ(
      fs().copy_file(root() / L"a.bin", root() / L"taken.bin", identity(L"a.bin")).error().code,
      Errc::already_exists);
  EXPECT_EQ(fs().copy_file(root() / L"dir", root() / L"d2", identity(L"dir")).error().code,
            Errc::invalid_argument);
  EXPECT_EQ(fs().copy_file(root() / L"link", root() / L"l2", identity(L"link")).error().code,
            Errc::invalid_argument);
  EXPECT_EQ(fs().copy_file(root() / L"cloud.bin", root() / L"c2.bin", identity(L"cloud.bin"))
                .error()
                .code,
            Errc::invalid_argument);
  EXPECT_EQ(
      fs().copy_file(root() / L"gone.bin", root() / L"g2.bin", identity(L"a.bin")).error().code,
      Errc::not_found);
  for (const wchar_t* name : {L"b.bin", L"d2", L"l2", L"c2.bin", L"g2.bin"}) {
    EXPECT_FALSE(entry_exists(root() / name)) << name;
  }
  EXPECT_EQ(fs().stat(root() / L"taken.bin")->size, Bytes{10});
}

TEST_F(Win32FileSystemTest, CopyFileOfAFileBeingWrittenIsInUse) {
  write_file(root() / L"log.txt", 10);
  const FileIdentity identity = fs().stat(root() / L"log.txt")->identity;
  const wil::unique_hfile writer{::CreateFileW((root() / L"log.txt").c_str(), GENERIC_WRITE,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                               OPEN_EXISTING, 0, nullptr)};
  ASSERT_TRUE(writer);
  EXPECT_EQ(fs().copy_file(root() / L"log.txt", root() / L"copy.txt", identity).error().code,
            Errc::in_use);
  EXPECT_FALSE(entry_exists(root() / L"copy.txt"));
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
