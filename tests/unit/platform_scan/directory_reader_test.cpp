#include "platform/scan/directory_reader.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stop_token>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"
#include "core/space/text.hpp"
#include "sandbox.hpp"

namespace myclean::platform {
namespace {

namespace fs = std::filesystem;
using space::FileItem;
using space::Folder;
using space::ScanResult;

const FileItem* find_file(const ScanResult& scan, const fs::path& path) {
  for (const Folder& folder : scan.folders) {
    for (const FileItem& file : folder.files) {
      if (space::path_of(folder, file) == path) {
        return &file;
      }
    }
  }
  return nullptr;
}

ScanResult scan_ok(const fs::path& root, unsigned workers = 4) {
  const Win32DirectoryReader reader;
  Result<ScanResult> result = space::scan(reader, root, space::ScanOptions{.workers = workers});
  if (!result) {
    throw std::runtime_error(result.error().message);
  }
  return std::move(*result);
}

// Restores listing rights on a folder at the end of a test, so that the sandbox can be removed.
class ListingDenied {
 public:
  explicit ListingDenied(fs::path path)
      : path_(std::move(path)), denied_(deny_listing(path_, true)) {}
  ~ListingDenied() { static_cast<void>(deny_listing(path_, false)); }
  ListingDenied(const ListingDenied&) = delete;
  ListingDenied& operator=(const ListingDenied&) = delete;
  ListingDenied(ListingDenied&&) = delete;
  ListingDenied& operator=(ListingDenied&&) = delete;

  [[nodiscard]] bool denied() const { return denied_; }

 private:
  fs::path path_;
  bool denied_;
};

TEST(Win32DirectoryReader, ScansATreeWithSizesTimesAndIdentities) {
  const Sandbox sandbox;
  write_file(sandbox.root() / "a.bin", 10'000);
  write_file(sandbox.root() / "sub" / "b.bin", 5000);
  write_file(sandbox.root() / "sub" / "deep" / "c.bin", 1);

  const ScanResult result = scan_ok(sandbox.root());

  EXPECT_EQ(result.root().file_count, 3U);
  EXPECT_EQ(result.root().folder_count, 2U);
  EXPECT_EQ(result.root().size, Bytes{15'001});
  EXPECT_EQ(result.error_count, 0U);
  const FileItem* a = find_file(result, sandbox.root() / "a.bin");
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->size, Bytes{10'000});
  EXPECT_GE(a->size_on_disk, Bytes{10'000});
  EXPECT_NE(a->identity, FileIdentity{});
  const auto age = std::chrono::system_clock::now() - a->modified;
  EXPECT_LT(age, std::chrono::minutes{5});
  EXPECT_GT(age, -std::chrono::minutes{5});
}

TEST(Win32DirectoryReader, ListsAJunctionAsALinkAndNeverCountsItsTarget) {
  const Sandbox sandbox;
  write_file(sandbox.root() / "store" / "package.tgz", 1'000'000);
  write_file(sandbox.root() / "project" / "package.json", 100);
  ASSERT_TRUE(make_junction(sandbox.root() / "project" / "node_modules", sandbox.root() / "store"));

  const ScanResult project = scan_ok(sandbox.root() / "project");
  const ScanResult whole = scan_ok(sandbox.root());

  EXPECT_EQ(project.folders.size(), 1U);
  EXPECT_EQ(project.link_count, 1U);
  EXPECT_EQ(project.root().size, Bytes{100});
  const FileItem* link = find_file(project, sandbox.root() / "project" / "node_modules");
  ASSERT_NE(link, nullptr);
  EXPECT_TRUE(link->link);
  EXPECT_EQ(link->reparse_tag, IO_REPARSE_TAG_MOUNT_POINT);
  EXPECT_EQ(whole.root().size, Bytes{1'000'100});  // the store once, through its real folder
}

TEST(Win32DirectoryReader, RefusesToListAJunction) {
  const Sandbox sandbox;
  fs::create_directories(sandbox.root() / "target");
  ASSERT_TRUE(make_junction(sandbox.root() / "link", sandbox.root() / "target"));
  const Win32DirectoryReader reader;

  const auto listed = reader.list(sandbox.root() / "link");

  ASSERT_FALSE(listed.has_value());
  EXPECT_EQ(listed.error().code, Errc::invalid_argument);
}

TEST(Win32DirectoryReader, ScansPathsLongerThanMaxPath) {
  const Sandbox sandbox;
  fs::path deep = sandbox.root();
  for (int i = 0; i < 12; ++i) {
    deep /= std::wstring(30, static_cast<wchar_t>(L'a' + i));
  }
  ASSERT_GT(deep.native().size(), 300U);
  write_file(deep / "deep.bin", 123);

  const ScanResult result = scan_ok(sandbox.root());

  EXPECT_EQ(result.error_count, 0U);
  const FileItem* file = find_file(result, deep / "deep.bin");
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->size, Bytes{123});
}

TEST(Win32DirectoryReader, ReportsSparseAndCompressedFilesAtTheirSizeOnDisk) {
  const Sandbox sandbox;
  write_file(sandbox.root() / "sparse.bin", 0);
  ASSERT_TRUE(make_sparse(sandbox.root() / "sparse.bin", 100'000'000));
  write_file(sandbox.root() / "ntfs.bin", 1'000'000);
  ASSERT_TRUE(compress_ntfs(sandbox.root() / "ntfs.bin"));

  const ScanResult result = scan_ok(sandbox.root());

  const FileItem* sparse = find_file(result, sandbox.root() / "sparse.bin");
  ASSERT_NE(sparse, nullptr);
  EXPECT_EQ(sparse->size, Bytes{100'000'000});
  EXPECT_LT(sparse->size_on_disk, Bytes{1'000'000});
  const FileItem* ntfs = find_file(result, sandbox.root() / "ntfs.bin");
  ASSERT_NE(ntfs, nullptr);
  EXPECT_EQ(ntfs->size, Bytes{1'000'000});
  EXPECT_LT(ntfs->size_on_disk, Bytes{500'000});
}

TEST(Win32DirectoryReader, AsksTheFileForTheSizeOfAWindowsCompressedFile) {
  const Sandbox sandbox;
  write_file(sandbox.root() / "wof.bin", 1'000'000);
  if (!compress_wof(sandbox.root() / "wof.bin")) {
    GTEST_SKIP() << "WOF compression is not available on this volume";
  }

  const ScanResult result = scan_ok(sandbox.root());

  const FileItem* wof = find_file(result, sandbox.root() / "wof.bin");
  ASSERT_NE(wof, nullptr);
  EXPECT_FALSE(wof->link);
  EXPECT_EQ(wof->size, Bytes{1'000'000});
  EXPECT_GT(wof->size_on_disk, Bytes{});  // the listing alone says 0
  EXPECT_LT(wof->size_on_disk, Bytes{500'000});
}

TEST(Win32DirectoryReader, MarksAnOfflineFileAsAPlaceholder) {
  const Sandbox sandbox;
  write_file(sandbox.root() / "cloud.docx", 2000);
  ASSERT_TRUE(mark_offline(sandbox.root() / "cloud.docx"));

  const ScanResult result = scan_ok(sandbox.root());

  const FileItem* cloud = find_file(result, sandbox.root() / "cloud.docx");
  ASSERT_NE(cloud, nullptr);
  EXPECT_TRUE(cloud->placeholder);
  EXPECT_EQ(result.placeholder_count, 1U);
}

TEST(Win32DirectoryReader, RecordsAFolderItCannotListAndGoesOn) {
  const Sandbox sandbox;
  write_file(sandbox.root() / "open" / "a.bin", 10);
  fs::create_directories(sandbox.root() / "locked");
  const ListingDenied locked{sandbox.root() / "locked"};
  ASSERT_TRUE(locked.denied());

  const ScanResult result = scan_ok(sandbox.root());

  EXPECT_EQ(result.error_count, 1U);
  EXPECT_EQ(result.root().size, Bytes{10});
  for (const Folder& folder : result.folders) {
    if (folder.path == sandbox.root() / "locked") {
      ASSERT_TRUE(folder.error.has_value());
      EXPECT_EQ(folder.error.value_or(Error{}).code, Errc::access_denied);
    }
  }
}

TEST(Win32DirectoryReader, ScansASandboxOfManyFilesQuickly) {
  const Sandbox sandbox;
  constexpr int folders = 40;
  constexpr int files_per_folder = 100;
  for (int f = 0; f < folders; ++f) {
    for (int i = 0; i < files_per_folder; ++i) {
      write_file(sandbox.root() / std::to_wstring(f) / (std::to_wstring(i) + L".txt"), 10);
    }
  }

  const auto start = std::chrono::steady_clock::now();
  const ScanResult result = scan_ok(sandbox.root());
  const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_EQ(result.root().file_count, static_cast<std::uint64_t>(folders) * files_per_folder);
  std::cout << "[ sandbox ] " << result.root().file_count << " files in " << elapsed.count()
            << " s = " << static_cast<double>(result.root().file_count) / elapsed.count()
            << " files/s\n";
}

// Read-only smoke test on a real system folder, capped at 10 s.
TEST(Win32DirectoryReader, SmokeScansTheDriversFolderReadOnly) {
  std::wstring windows(MAX_PATH, L'\0');
  windows.resize(::GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size())));
  const fs::path drivers = fs::path{windows} / L"System32" / L"drivers";
  const Win32DirectoryReader reader;
  std::stop_source stop;
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + std::chrono::seconds{10};

  const Result<ScanResult> result =
      space::scan(reader, drivers, space::ScanOptions{.workers = 2, .progress_every = 200},
                  stop.get_token(), [&](const space::ScanProgress&) {
                    if (std::chrono::steady_clock::now() > deadline) {
                      stop.request_stop();
                    }
                  });
  const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_TRUE(result.has_value()) << result.error().message;
  EXPECT_GT(result->root().file_count, 0U);
  EXPECT_GT(result->root().size_on_disk, Bytes{});
  std::cout << "[ drivers ] " << result->root().file_count << " files, "
            << result->root().folder_count << " folders, " << result->error_count << " unreadable, "
            << result->link_count << " links in " << elapsed.count()
            << " s = " << static_cast<double>(result->root().file_count) / elapsed.count()
            << " files/s" << (result->cancelled ? " (capped)" : "") << "\n";
}

}  // namespace
}  // namespace myclean::platform
