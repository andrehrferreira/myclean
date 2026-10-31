#include "fakes/fake_file_system.hpp"

#include <algorithm>
#include <filesystem>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/file_identity.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"

namespace myclean::fakes {
namespace {

using namespace myclean::literals;
using std::filesystem::path;

constexpr os::FileAttributes reparse{.reparse_point = true};

std::vector<path> names_in(const FakeFileSystem& fs, const path& directory) {
  std::vector<path> names;
  const Result<std::vector<os::DirEntry>> entries = fs.enumerate(directory);
  for (const os::DirEntry& entry : entries.value()) {
    names.push_back(entry.path.filename());
  }
  std::ranges::sort(names);
  return names;
}

TEST(FakeFileSystem, StatIsCaseInsensitiveAndDescribesTheEntry) {
  FakeFileSystem fs;
  const FileIdentity id = fs.add_file(L"C:\\Temp\\Setup.log", 4_KiB);
  const Result<os::FileStat> stat = fs.stat(L"c:\\temp\\SETUP.LOG");
  ASSERT_TRUE(stat.has_value());
  EXPECT_EQ(stat->kind, os::EntryKind::file);
  EXPECT_EQ(stat->size, 4_KiB);
  EXPECT_EQ(stat->size_on_disk, 4_KiB);
  EXPECT_EQ(stat->identity, id);
  EXPECT_EQ(fs.stat(L"C:\\Temp")->kind, os::EntryKind::directory);
  EXPECT_EQ(fs.stat(L"C:\\Temp\\missing").error().code, Errc::not_found);
  EXPECT_EQ(fs.stat(L"Temp\\Setup.log").error().code, Errc::invalid_argument);
}

TEST(FakeFileSystem, CloudPlaceholderHasNoSizeOnDisk) {
  FakeFileSystem fs;
  fs.add_file(L"C:\\Users\\owner\\OneDrive\\big.iso", 4_GiB, {.recall_on_data_access = true});
  const os::FileStat stat = fs.stat(L"C:\\Users\\owner\\OneDrive\\big.iso").value();
  EXPECT_TRUE(stat.attributes.cloud_placeholder());
  EXPECT_EQ(stat.size, 4_GiB);
  EXPECT_EQ(stat.size_on_disk, Bytes{});
}

TEST(FakeFileSystem, EnumerateListsDirectChildrenOnly) {
  FakeFileSystem fs;
  fs.add_file(L"F:\\proj\\a.txt", 1_B);
  fs.add_file(L"F:\\proj\\sub\\b.txt", 1_B);
  fs.add_file(L"F:\\project2\\c.txt", 1_B);
  EXPECT_EQ(names_in(fs, L"F:\\proj"), (std::vector<path>{L"a.txt", L"sub"}));
  EXPECT_EQ(fs.enumerate(L"F:\\proj\\a.txt").error().code, Errc::invalid_argument);
}

TEST(FakeFileSystem, EnumerateRefusesReparsePoints) {
  FakeFileSystem fs;
  fs.add_directory(L"F:\\proj\\node_modules\\pkg", reparse);
  const auto entries = fs.enumerate(L"F:\\proj\\node_modules\\pkg");
  ASSERT_FALSE(entries.has_value());
  EXPECT_EQ(entries.error().code, Errc::invalid_argument);
}

TEST(FakeFileSystem, RemoveDoesNotRecurseAndRemovesLinksAsLinks) {
  FakeFileSystem fs;
  fs.add_file(L"F:\\store\\pkg\\index.js", 1_KiB);
  fs.add_directory(L"F:\\proj\\node_modules\\pkg", reparse);
  EXPECT_EQ(fs.remove(L"F:\\proj\\node_modules").error().code, Errc::not_empty);

  EXPECT_TRUE(fs.remove(L"F:\\proj\\node_modules\\pkg").has_value());
  EXPECT_TRUE(fs.remove(L"F:\\proj\\node_modules").has_value());
  EXPECT_FALSE(fs.exists(L"F:\\proj\\node_modules"));
  EXPECT_TRUE(fs.exists(L"F:\\store\\pkg\\index.js"));
}

TEST(FakeFileSystem, RenameMovesSubtreeAndKeepsIdentity) {
  FakeFileSystem fs;
  const FileIdentity id = fs.add_file(L"D:\\cache\\x\\data.bin", 1_MiB);
  fs.add_directory(L"D:\\.myclean-quarantine\\run-1");
  ASSERT_TRUE(fs.rename(L"D:\\cache\\x", L"D:\\.myclean-quarantine\\run-1\\x").has_value());
  EXPECT_FALSE(fs.exists(L"D:\\cache\\x"));
  const auto moved = fs.stat(L"D:\\.myclean-quarantine\\run-1\\x\\data.bin");
  ASSERT_TRUE(moved.has_value());
  EXPECT_EQ(moved->identity, id);
  EXPECT_EQ(names_in(fs, L"D:\\.myclean-quarantine\\run-1\\x"), (std::vector<path>{L"data.bin"}));
}

TEST(FakeFileSystem, RenameRejectsExistingTargetOtherVolumeAndMissingParent) {
  FakeFileSystem fs;
  fs.add_file(L"C:\\a.txt", 1_B);
  fs.add_file(L"C:\\b.txt", 1_B);
  fs.add_directory(L"L:\\");
  EXPECT_EQ(fs.rename(L"C:\\a.txt", L"C:\\B.TXT").error().code, Errc::already_exists);
  EXPECT_EQ(fs.rename(L"C:\\a.txt", L"L:\\a.txt").error().code, Errc::invalid_argument);
  EXPECT_EQ(fs.rename(L"C:\\a.txt", L"C:\\no\\a.txt").error().code, Errc::not_found);
  EXPECT_TRUE(fs.exists(L"C:\\a.txt"));
}

TEST(FakeFileSystem, MoveFileAcrossVolumesCreatesANewFile) {
  FakeFileSystem fs;
  const FileIdentity id = fs.add_file(L"C:\\big.iso", 2_GiB);
  fs.add_directory(L"L:\\quarantine");
  ASSERT_TRUE(fs.move_file(L"C:\\big.iso", L"L:\\quarantine\\big.iso").has_value());
  EXPECT_FALSE(fs.exists(L"C:\\big.iso"));
  const os::FileStat moved = fs.stat(L"L:\\quarantine\\big.iso").value();
  EXPECT_EQ(moved.size, 2_GiB);
  EXPECT_NE(moved.identity, id);
  EXPECT_EQ(fs.move_file(L"L:\\quarantine", L"C:\\q").error().code, Errc::invalid_argument);
}

TEST(FakeFileSystem, CreateDirectoriesCreatesMissingParents) {
  FakeFileSystem fs;
  fs.add_file(L"D:\\file", 1_B);
  ASSERT_TRUE(fs.create_directories(L"D:\\.myclean-quarantine\\run-7").has_value());
  EXPECT_EQ(fs.stat(L"D:\\.myclean-quarantine\\run-7")->kind, os::EntryKind::directory);
  EXPECT_TRUE(fs.create_directories(L"D:\\.myclean-quarantine").has_value());
  EXPECT_EQ(fs.create_directories(L"D:\\file\\sub").error().code, Errc::invalid_argument);
  EXPECT_EQ(fs.create_directories(L"Q:\\x").error().code, Errc::not_found);
}

TEST(FakeFileSystem, ExpandShortNamesAndNormalizesDots) {
  FakeFileSystem fs;
  fs.add_file(L"C:\\Program Files\\App\\app.exe", 1_MiB);
  fs.set_short_name(L"C:\\Program Files", L"PROGRA~1");
  const auto expanded = fs.expand_short_names(L"C:\\PROGRA~1\\app\\.\\..\\App\\APP.EXE");
  ASSERT_TRUE(expanded.has_value());
  EXPECT_EQ(*expanded, path{L"C:\\Program Files\\App\\app.exe"});
  EXPECT_EQ(fs.expand_short_names(L"C:\\PROGRA~2\\App").error().code, Errc::not_found);
}

TEST(FakeFileSystem, OutsideChangeGivesTheSamePathANewIdentity) {
  FakeFileSystem fs;
  const FileIdentity before = fs.add_directory(L"F:\\proj\\target");
  fs.erase(L"F:\\proj\\target");
  const FileIdentity after = fs.add_directory(L"F:\\proj\\target", reparse);
  EXPECT_NE(before, after);
  EXPECT_TRUE(fs.stat(L"F:\\proj\\target")->attributes.reparse_point);
  EXPECT_TRUE(fs.operations().empty());
}

TEST(FakeFileSystem, RecordsEveryMutatingCallAndInjectsFailures) {
  FakeFileSystem fs;
  fs.add_file(L"C:\\Temp\\locked.tmp", 1_B);
  fs.fail_on(L"C:\\Temp\\locked.tmp", Errc::in_use);
  EXPECT_EQ(fs.remove(L"C:\\Temp\\locked.tmp").error().code, Errc::in_use);
  EXPECT_TRUE(fs.exists(L"C:\\Temp\\locked.tmp"));
  EXPECT_TRUE(fs.create_directories(L"C:\\Temp\\new").has_value());

  using Kind = FakeFileSystem::OperationKind;
  EXPECT_EQ(fs.operations(), (std::vector<FakeFileSystem::Operation>{
                                 {.kind = Kind::remove, .from = L"C:\\Temp\\locked.tmp"},
                                 {.kind = Kind::create_directories, .from = L"C:\\Temp\\new"}}));
}

}  // namespace
}  // namespace myclean::fakes
