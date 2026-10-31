#include "core/space/json.hpp"

#include <chrono>
#include <filesystem>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/downloads.hpp"
#include "core/space/duplicates.hpp"
#include "core/space/installers.hpp"
#include "core/space/scanner.hpp"
#include "core/space/top.hpp"
#include "space_fakes.hpp"

namespace myclean::space {
namespace {

using testing::FakeDirectoryReader;
using testing::file;
using testing::scan_ok;
namespace fs = std::filesystem;

const WallTime day_one = std::chrono::sys_days{std::chrono::year{1970} / 1 / 2};

TEST(SpaceJson, SummarizesAScan) {
  FakeDirectoryReader reader;
  const fs::path root{L"C:\\caf\u00e9"};
  reader.listings[root] = {file("a", 10, 1), testing::junction("j"), testing::folder("x")};
  reader.errors.insert_or_assign(root / "x",
                                 Error{.code = Errc::access_denied, .message = "denied"});

  const nlohmann::json json = scan_ok(reader, root);

  EXPECT_EQ(json["root"], "C:\\caf\xC3\xA9");
  EXPECT_EQ(json["size_on_disk"], 10);
  EXPECT_EQ(json["files"], 1);
  EXPECT_EQ(json["folders"], 1);
  EXPECT_EQ(json["links"], 1);
  EXPECT_EQ(json["placeholders"], 0);
  EXPECT_EQ(json["cancelled"], false);
  ASSERT_EQ(json["unreadable"].size(), 1U);
  EXPECT_EQ(json["unreadable"][0]["path"], "C:\\caf\xC3\xA9\\x");
  EXPECT_EQ(json["unreadable"][0]["error"]["code"], "access_denied");
}

TEST(SpaceJson, WritesTopItems) {
  const TopItems top{
      .files = {{.path = L"C:\\a.bin",
                 .size_on_disk = Bytes{4096},
                 .size = Bytes{4000},
                 .modified = day_one}},
      .folders = {
          {.path = L"C:\\dir", .size_on_disk = Bytes{8192}, .size = Bytes{8000}, .file_count = 2}}};

  const nlohmann::json json = top;

  EXPECT_EQ(json["files"][0]["path"], "C:\\a.bin");
  EXPECT_EQ(json["files"][0]["size_on_disk"], 4096);
  EXPECT_EQ(json["files"][0]["size"], 4000);
  EXPECT_EQ(json["files"][0]["modified"], 86400);
  EXPECT_EQ(json["folders"][0]["files"], 2);
}

TEST(SpaceJson, WritesOldDownloadsWithAndWithoutOrigin) {
  const OldDownload known{.path = L"C:\\d\\setup.zip",
                          .size = Bytes{5},
                          .size_on_disk = Bytes{4096},
                          .created = day_one,
                          .modified = day_one,
                          .age_days = 120,
                          .origin = Origin{.zone_id = 3, .host_url = "https://h/s.zip"}};
  const OldDownload unknown{.path = L"C:\\d\\b"};

  const nlohmann::json json = known;
  const nlohmann::json other = unknown;

  EXPECT_EQ(json["age_days"], 120);
  EXPECT_EQ(json["created"], 86400);
  EXPECT_EQ(json["origin"]["zone_id"], 3);
  EXPECT_EQ(json["origin"]["host_url"], "https://h/s.zip");
  EXPECT_TRUE(json["origin"]["referrer_url"].is_null());
  EXPECT_TRUE(other["origin"].is_null());
}

TEST(SpaceJson, WritesInstallersAndDuplicates) {
  const InstallerReport installers{
      .used = {{.path = L"C:\\d\\t.msi",
                .kind = InstallerKind::msi,
                .size = Bytes{3},
                .size_on_disk = Bytes{4096},
                .product_code = "{1}",
                .installed_as = "Tool"},
               {.path = L"C:\\d\\a.exe",
                .kind = InstallerKind::exe,
                .installed_as = "App",
                .publisher = "Vendor"}},
      .errors = {
          {.path = L"C:\\d\\x.msi", .error = Error{.code = Errc::invalid_data, .message = "bad"}}}};
  const DuplicateReport duplicates{
      .groups = {{.size = Bytes{100},
                  .digest = Digest{},
                  .reclaimable = Bytes{4096},
                  .files = {{.path = L"D:\\a", .size_on_disk = Bytes{4096}},
                            {.path = L"K:\\b", .size_on_disk = Bytes{4096}}}}}};

  const nlohmann::json used = installers;
  const nlohmann::json dupes = duplicates;

  EXPECT_EQ(used["used"][0]["kind"], "msi");
  EXPECT_EQ(used["used"][0]["product_code"], "{1}");
  EXPECT_FALSE(used["used"][0].contains("publisher"));
  EXPECT_EQ(used["used"][1]["publisher"], "Vendor");
  EXPECT_EQ(used["errors"][0]["error"]["code"], "invalid_data");
  EXPECT_EQ(dupes["groups"][0]["hash"], std::string(64, '0'));
  EXPECT_EQ(dupes["groups"][0]["files"].size(), 2U);
  EXPECT_EQ(dupes["reclaimable"], 4096);
  EXPECT_EQ(dupes["cancelled"], false);
}

}  // namespace
}  // namespace myclean::space
