#include "core/scanall/duplicates.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/health/analysis.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/scanall/findings.hpp"
#include "core/space/duplicates.hpp"
#include "core/space/scanner.hpp"
#include "scanall/scanall_fakes.hpp"
#include "space/space_fakes.hpp"

namespace myclean::scanall {
namespace {

namespace fs = std::filesystem;
using namespace myclean::literals;
using placement::DriveClass;
using space::testing::FakeDirectoryReader;
using space::testing::scan_ok;
using testing::drive;

const WallTime older = std::chrono::sys_days{std::chrono::year{2025} / 3 / 1};
const WallTime newer = std::chrono::sys_days{std::chrono::year{2026} / 9 / 1};

// Hashes in-memory content (std::hash in the first bytes of the digest).
class FakeHasher final : public space::IContentHasher {
 public:
  std::map<fs::path, std::string> contents;

  [[nodiscard]] Result<space::Digest> hash(const fs::path& file,
                                           std::optional<Bytes> limit) const override {
    const auto content = contents.find(file);
    if (content == contents.end()) {
      return fail(Errc::in_use, "locked");
    }
    const std::string part =
        limit ? content->second.substr(0, static_cast<std::size_t>(limit->count()))
              : content->second;
    space::Digest digest{};
    const std::size_t value = std::hash<std::string>{}(part);
    for (std::size_t i = 0; i < sizeof(value); ++i) {
      digest.at(i) = static_cast<std::uint8_t>(value >> (8 * i));
    }
    return digest;
  }
};

// One drive's folder tree; every file gets its own id unless one is given.
struct Volume {
  fs::path root;
  std::uint64_t serial = 0;
  FakeDirectoryReader reader;
  std::uint8_t next_id = 1;

  void add(FakeHasher& hasher, const fs::path& folder, const std::string& name,
           const std::string& content, WallTime time, std::optional<std::uint8_t> id = {}) {
    for (fs::path path = folder; path != root && path.has_relative_path();
         path = path.parent_path()) {
      std::vector<space::ListedEntry>& parent = reader.listings[path.parent_path()];
      const std::string leaf = path.filename().string();
      if (std::ranges::find(parent, fs::path(leaf), &space::ListedEntry::name) == parent.end()) {
        parent.push_back(testing::folder(leaf));
      }
    }
    reader.listings[folder].push_back(
        testing::file(name, Bytes{content.size()}, time, id.value_or(next_id++), serial));
    hasher.contents[folder / name] = content;
  }

  [[nodiscard]] space::ScanResult tree() {
    reader.listings.try_emplace(root);
    return scan_ok(reader, root);
  }
};

TEST(CrossDuplicates, KeepsTheCopyInALibraryOnAHealthyDrive) {
  FakeHasher hasher;
  Volume c{.root = LR"(C:\)", .serial = 3};
  Volume d{.root = LR"(D:\)", .serial = 4};
  Volume k{.root = LR"(K:\)", .serial = 11};
  const std::string photo(300, 'p');
  c.add(hasher, LR"(C:\Users\u\Downloads)", "p.jpg", photo, older);
  d.add(hasher, LR"(D:\Docs)", "p.jpg", photo, older);
  k.add(hasher, LR"(K:\Backup)", "p.jpg", photo, newer);
  const std::vector<space::ScanResult> trees = {c.tree(), d.tree(), k.tree()};
  const std::vector<DriveInfo> drives = {
      drive(LR"(C:\)", DriveClass::nvme), drive(LR"(D:\)", DriveClass::nvme),
      drive(LR"(K:\)", DriveClass::hdd, health::Status::warning)};

  const DuplicateRun run = find_duplicates(
      trees, drives, hasher,
      {.min_size = 1_B, .preferred = {LR"(D:\Docs)"}, .transient = {LR"(C:\Users\u\Downloads)"}});

  EXPECT_EQ(run.groups, 1U);
  ASSERT_EQ(run.findings.size(), 2U);
  for (const Finding& finding : run.findings) {
    EXPECT_EQ(finding.group, ActionGroup::deduplicate);
    EXPECT_EQ(finding.plan.kind, PlanKind::quarantine_items);
    EXPECT_EQ(*finding.plan.param("keep"), R"(D:\Docs\p.jpg)");
    ASSERT_EQ(finding.items.size(), 1U);
    EXPECT_EQ(finding.bytes, Bytes{photo.size()});
  }
  EXPECT_EQ(run.findings[0].drive, fs::path(LR"(C:\)"));
  EXPECT_EQ(run.findings[1].items[0].path, fs::path(LR"(K:\Backup\p.jpg)"));
}

TEST(CrossDuplicates, NewestCopyWinsOtherwiseAndHardLinksAreOneFile) {
  FakeHasher hasher;
  Volume d{.root = LR"(D:\)", .serial = 4};
  const std::string data(200, 'x');
  d.add(hasher, LR"(D:\a)", "x.bin", data, older);
  d.add(hasher, LR"(D:\b)", "x.bin", data, newer);
  d.add(hasher, LR"(D:\h)", "one.bin", std::string(150, 'h'), older, 200);
  d.add(hasher, LR"(D:\h)", "two.bin", std::string(150, 'h'), older, 200);
  const std::vector<space::ScanResult> trees = {d.tree()};
  const std::vector<DriveInfo> drives = {drive(LR"(D:\)", DriveClass::nvme)};

  const DuplicateRun run = find_duplicates(trees, drives, hasher, {.min_size = 1_B});

  ASSERT_EQ(run.findings.size(), 1U);
  EXPECT_EQ(run.findings[0].path, fs::path(LR"(D:\a\x.bin)"));
  EXPECT_EQ(*run.findings[0].plan.param("keep"), R"(D:\b\x.bin)");
}

TEST(CrossDuplicates, StopsAtTheHashBudgetLargestFirst) {
  FakeHasher hasher;
  Volume d{.root = LR"(D:\)", .serial = 4};
  d.add(hasher, LR"(D:\big)", "1", std::string(100, 'b'), older);
  d.add(hasher, LR"(D:\big)", "2", std::string(100, 'b'), older);
  d.add(hasher, LR"(D:\small)", "1", std::string(10, 's'), older);
  d.add(hasher, LR"(D:\small)", "2", std::string(10, 's'), older);
  const std::vector<space::ScanResult> trees = {d.tree()};
  const std::vector<DriveInfo> drives = {drive(LR"(D:\)", DriveClass::nvme)};

  const DuplicateRun run =
      find_duplicates(trees, drives, hasher, {.min_size = 1_B, .budget = 215_B});

  EXPECT_EQ(run.skipped, 1U);
  EXPECT_EQ(run.hashed, 210_B);
  EXPECT_EQ(run.groups, 1U);
  EXPECT_EQ(run.errors, 0U);
  ASSERT_EQ(run.findings.size(), 1U);
  EXPECT_EQ(run.findings[0].bytes, 100_B);
}

TEST(CrossDuplicates, FilesTooLargeForTheBudgetAreSkippedNotTheSearch) {
  FakeHasher hasher;
  Volume d{.root = LR"(D:\)", .serial = 4};
  d.add(hasher, LR"(D:\big)", "1", std::string(300U << 10U, 'b'), older);
  d.add(hasher, LR"(D:\big)", "2", std::string(300U << 10U, 'b'), older);
  d.add(hasher, LR"(D:\small)", "1", std::string(10U << 10U, 's'), older);
  d.add(hasher, LR"(D:\small)", "2", std::string(10U << 10U, 's'), older);
  const std::vector<space::ScanResult> trees = {d.tree()};
  const std::vector<DriveInfo> drives = {drive(LR"(D:\)", DriveClass::nvme)};

  const DuplicateRun run =
      find_duplicates(trees, drives, hasher, {.min_size = 1_B, .budget = 200_KiB});

  EXPECT_EQ(run.skipped, 2U);      // the full hashes of the 300 KiB files
  EXPECT_EQ(run.hashed, 148_KiB);  // their two head hashes and the small files
  EXPECT_EQ(run.groups, 1U);
  ASSERT_EQ(run.findings.size(), 1U);
  EXPECT_EQ(run.findings[0].bytes, 10_KiB);
}

}  // namespace
}  // namespace myclean::scanall
