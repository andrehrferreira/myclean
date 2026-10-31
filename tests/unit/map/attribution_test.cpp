#include "core/map/attribution.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/placement/relocation.hpp"
#include "core/rules/catalog.hpp"
#include "core/rules/data_files.hpp"

namespace myclean::map {
namespace {

const std::filesystem::path file = R"(C:\MyClean\data\attribution.toml)";

constexpr std::string_view valid_area = R"(
[[area]]
id       = "windows.hibernation"
paths    = ['?:\hiberfil.sys', '%SystemDrive%\other']
category = "page-hibernate"
label    = "Hibernation file"
reclaim  = { kind = "setting", ref = "hibernation", note = "Reduce it." }
)";

// valid_area with `line` in place of its line with the same key.
std::string with(std::string_view line) {
  std::string text(valid_area);
  const std::string key = std::string(line.substr(0, line.find(' '))) + " ";
  const std::size_t start = text.find("\n" + key) + 1;
  text.replace(start, text.find('\n', start) - start, line);
  return text;
}

std::string rejection(std::string_view text) {
  const Result<std::vector<Area>> parsed = parse_attribution_file(text, file);
  EXPECT_FALSE(parsed.has_value()) << text;
  return parsed ? std::string{} : parsed.error().message;
}

TEST(ParseAttribution, ReadsEveryField) {
  const Result<std::vector<Area>> parsed = parse_attribution_file(valid_area, file);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  ASSERT_EQ(parsed->size(), 1U);
  const Area& area = parsed->front();
  EXPECT_EQ(area.id, "windows.hibernation");
  EXPECT_EQ(area.paths,
            (std::vector<std::wstring>{LR"(?:\hiberfil.sys)", LR"(%SystemDrive%\other)"}));
  EXPECT_EQ(area.category, Category::page_hibernate);
  EXPECT_EQ(area.label, "Hibernation file");
  EXPECT_EQ(area.reclaim.kind, ReclaimKind::setting);
  EXPECT_EQ(area.reclaim.ref, "hibernation");
  EXPECT_EQ(area.reclaim.note, "Reduce it.");
}

TEST(ParseAttribution, EnumTextsRoundTrip) {
  for (const std::string_view text : {"system", "windows-servicing", "dev-caches", "other"}) {
    ASSERT_TRUE(parse_category(text).has_value()) << text;
    EXPECT_EQ(to_string(*parse_category(text)), text);
  }
  for (const std::string_view text : {"none", "clean-rule", "relocate", "manual-review"}) {
    ASSERT_TRUE(parse_reclaim_kind(text).has_value()) << text;
    EXPECT_EQ(to_string(*parse_reclaim_kind(text)), text);
  }
  EXPECT_FALSE(parse_category("cache").has_value());
  EXPECT_TRUE(reclaimable(ReclaimKind::uninstall));
  EXPECT_FALSE(reclaimable(ReclaimKind::manual_review));
  EXPECT_FALSE(reclaimable(ReclaimKind::none));
}

TEST(ParseAttribution, RejectsTheWholeFileNamingAreaAndField) {
  EXPECT_EQ(rejection(with("category = \"cache\"")),
            R"(C:\MyClean\data\attribution.toml: area #1 'windows.hibernation': field 'category' )"
            R"(must be one of system, windows-servicing, page-hibernate, restore-points, apps, )"
            R"(games, dev-toolchains, dev-caches, dev-projects, build-artifacts, browser-caches, )"
            R"(app-caches, vm-disks, user-documents, user-media, downloads, recycle-bin, other, )"
            R"(not 'cache')");
  EXPECT_NE(rejection(with("label    = \"\"")).find("field 'label' must be a non-empty string"),
            std::string::npos);
  EXPECT_NE(rejection(with("reclaim  = { kind = \"setting\" }"))
                .find("field 'reclaim.ref' is required for kind 'setting'"),
            std::string::npos);
  EXPECT_NE(rejection(with("reclaim  = { kind = \"none\", ref = \"x\" }"))
                .find("field 'reclaim.ref' is not allowed for kind 'none'"),
            std::string::npos);
  EXPECT_NE(rejection(with("reclaim  = { kind = \"setting\", ref = \"swap\" }"))
                .find("must be hibernation, page_file or shadow_storage, not 'swap'"),
            std::string::npos);
  EXPECT_NE(rejection(with("reclaim  = { kind = \"delete\" }")).find("field 'reclaim.kind'"),
            std::string::npos);
  EXPECT_NE(rejection(with("reclaim  = { kind = \"none\", why = \"x\" }"))
                .find("field 'reclaim.why' is not a known field"),
            std::string::npos);
  EXPECT_NE(rejection(std::string(valid_area) + "size = 3\n").find("field 'size' is not a known"),
            std::string::npos);
  EXPECT_NE(rejection(with("paths    = []")).find("field 'paths' must be a non-empty list"),
            std::string::npos);
  for (const std::string_view path : {R"(Windows\Temp)", R"(C:/Windows)", R"(C:\Windows\)",
                                      R"(C:\Windows\..\Users)", R"(C:\\Windows)", "C:"}) {
    EXPECT_NE(rejection(with("paths    = ['" + std::string(path) + "']"))
                  .find("field 'paths' must hold absolute paths"),
              std::string::npos)
        << path;
  }
  EXPECT_NE(rejection(std::string(valid_area) + std::string(valid_area))
                .find("area #2 'windows.hibernation': field 'id' repeats an id"),
            std::string::npos);
  EXPECT_NE(rejection("").find("field 'area' must be a non-empty array of tables"),
            std::string::npos);
  EXPECT_NE(rejection("[[area]\n").find("is not valid TOML"), std::string::npos);
  EXPECT_NE(rejection("[[area]]\nlabel = \"x\"\n").find("area #1: field 'id' is missing"),
            std::string::npos);
}

TEST(ShippedAttribution, LoadsAndEveryReferenceExists) {
  const rules::DiskDataFiles files;
  const Result<std::vector<Area>> areas = load_attribution(files, MYCLEAN_DATA_DIR);
  ASSERT_TRUE(areas.has_value()) << areas.error().message;
  EXPECT_GE(areas->size(), 60U);

  rules::RuleCatalog catalog = rules::make_rule_catalog(MYCLEAN_DATA_DIR);
  const std::vector<Error> errors = catalog.reload(files);
  ASSERT_TRUE(errors.empty()) << errors.front().message;
  for (const Area& area : *areas) {
    const Reclaim& reclaim = area.reclaim;
    if (reclaim.kind == ReclaimKind::clean_rule) {
      EXPECT_NE(catalog.find(reclaim.ref), nullptr) << area.id << ": no rule " << reclaim.ref;
    }
    if (reclaim.kind == ReclaimKind::relocate) {
      EXPECT_TRUE(placement::parse_cache_kind(reclaim.ref).has_value() || reclaim.ref == "wsl" ||
                  reclaim.ref == "docker")
          << area.id << ": no relocation " << reclaim.ref;
    }
  }
}

}  // namespace
}  // namespace myclean::map
