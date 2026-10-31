#include "core/packs/loader.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "core/map/attribution.hpp"
#include "core/model/result.hpp"
#include "core/packs/schema.hpp"
#include "pack_fakes.hpp"

namespace myclean::packs {
namespace {

namespace fs = std::filesystem;
using testing::FakePackFiles;

const fs::path builtin = R"(C:\MyClean\data\packs)";
const fs::path owner = R"(C:\ProgramData\MyClean\packs)";
const Version running{.major = 0, .minor = 1, .patch = 0};

std::string manifest(std::string_view id, std::string_view min_myclean = "0.1.0") {
  return "id = \"" + std::string(id) +
         "\"\nversion = \"1.2.3\"\ntitle = \"Title\"\ndescription = \"What it covers.\"\n"
         "sources = [\"observed on reference machine\"]\nmin_myclean = \"" +
         std::string(min_myclean) + "\"\n";
}

std::string rule(std::string_view id, std::string_view target) {
  return "[[rule]]\nid = \"" + std::string(id) +
         "\"\ncategory = \"dev-caches\"\ntier = \"ask\"\nscope = \"user\"\nstrategy = \"delete\"\n"
         "targets = ['" +
         std::string(target) + "']\n";
}

std::string area(std::string_view id, std::string_view path, std::string_view reclaim) {
  return "[[area]]\nid = \"" + std::string(id) + "\"\npaths = ['" + std::string(path) +
         "']\ncategory = \"dev-caches\"\nlabel = \"Label\"\nreclaim = " + std::string(reclaim) +
         "\n";
}

constexpr std::string_view recipe = R"([[relocate]]
id          = "home"
label       = "Tool home"
locations   = ['%USERPROFILE%\.tool']
current     = { env = "TOOL_HOME" }
target_kind = "env"
content     = "move"
undo        = "restore"
)";

// A valid pack `id` in `dir` with one clean case "cache" targeting `target`.
void put_pack(FakePackFiles& files, const fs::path& dir, std::string_view id,
              std::string_view target = R"(%LOCALAPPDATA%\tool\cache\*)") {
  files.put(dir / id / "pack.toml", manifest(id));
  files.put(dir / id / "clean.toml", rule("cache", target));
}

Result<Pack> load(const FakePackFiles& files, std::string_view id) {
  return load_pack(files, builtin / id, Origin::builtin, running);
}

void expect_error(const Result<Pack>& pack, std::string_view text) {
  ASSERT_FALSE(pack.has_value());
  EXPECT_NE(pack.error().message.find(text), std::string::npos) << pack.error().message;
}

TEST(LoadPack, NamespacesEveryCaseAndResolvesLocalRefs) {
  FakePackFiles files;
  const fs::path folder = builtin / "tool";
  files.put(folder / "pack.toml", manifest("tool"));
  files.put(folder / "clean.toml", rule("cache", R"(%LOCALAPPDATA%\tool\cache\*)"));
  files.put(folder / "relocate.toml", std::string(recipe));
  files.put(
      folder / "attribution.toml",
      area("cache", R"(%LOCALAPPDATA%\tool\cache)", R"({ kind = "clean-rule", ref = "cache" })") +
          area("home", R"(%USERPROFILE%\.tool)", R"({ kind = "relocate", ref = "home" })") +
          area("npm", R"(%LOCALAPPDATA%\npm-cache)", R"({ kind = "relocate", ref = "npm" })"));
  files.put(
      folder / "compress.toml",
      "[[compress]]\nid = \"bin\"\nlabel = \"Binaries\"\npaths = ['%USERPROFILE%\\.tool\\bin']\n"
      "algorithm = \"lzx\"\n");
  files.put(folder / "kb.json",
            R"({ "entries": [ { "id": "daemon", "image": "tool.exe", "description": "The tool.",
                "category": "developer", "needed_at_startup": false, "safe_to_remove": true } ] })");
  files.put(folder / "classes.toml",
            "[[match]]\nid = \"build\"\nclass = \"build\"\nimage = [\"tool.exe\"]\n");

  const Result<Pack> pack = load(files, "tool");
  ASSERT_TRUE(pack.has_value()) << pack.error().message;
  EXPECT_EQ(pack->manifest.version, (Version{.major = 1, .minor = 2, .patch = 3}));
  EXPECT_EQ(pack->origin, Origin::builtin);
  EXPECT_EQ(pack->clean.at(0).id, "tool/cache");
  EXPECT_EQ(pack->relocate.at(0).id, "tool/home");
  EXPECT_EQ(pack->compress.at(0).id, "tool/bin");
  EXPECT_EQ(pack->kb.at(0).id, "tool/daemon");
  EXPECT_EQ(pack->classes.at(0).id, "tool/build");
  ASSERT_EQ(pack->attribution.size(), 3U);
  EXPECT_EQ(pack->attribution[0].id, "tool/cache");
  EXPECT_EQ(pack->attribution[0].reclaim.ref, "tool/cache");
  EXPECT_EQ(pack->attribution[1].reclaim.ref, "tool/home");
  EXPECT_EQ(pack->attribution[2].reclaim.ref, "npm");  // a built-in relocation target
}

TEST(LoadPack, ErrorsNamePackFileCaseAndField) {
  FakePackFiles files;
  files.put(builtin / "tool" / "pack.toml", manifest("tool"));
  files.put(builtin / "tool" / "clean.toml",
            "[[rule]]\nid = \"cache\"\ncategory = \"x\"\nscope = \"user\"\nstrategy = \"delete\"\n"
            "targets = ['%TEMP%\\x']\n");
  const Result<Pack> pack = load(files, "tool");
  expect_error(pack, "pack 'tool': ");
  expect_error(pack, "clean.toml: rule #1 'cache': field 'tier' is missing");
  EXPECT_EQ(pack.error().code, Errc::invalid_data);
}

TEST(LoadPack, RejectsFolderLayoutProblems) {
  FakePackFiles stray;
  put_pack(stray, builtin, "tool");
  stray.put(builtin / "tool" / "README.md", "notes");
  expect_error(load(stray, "tool"), "README.md: is not a pack file");

  FakePackFiles renamed;
  put_pack(renamed, builtin, "tool");
  renamed.put(builtin / "tool" / "pack.toml", manifest("other"));
  expect_error(load(renamed, "tool"), "field 'id' must be the folder's name, not 'other'");

  FakePackFiles empty;
  empty.put(builtin / "tool" / "pack.toml", manifest("tool"));
  expect_error(load(empty, "tool"), "has no content file");

  FakePackFiles unnamed;
  unnamed.put(builtin / "tool" / "clean.toml", rule("cache", R"(%TEMP%\x)"));
  expect_error(load(unnamed, "tool"), "has no pack.toml");
}

TEST(LoadPack, RejectsBadCaseIdsAndRefs) {
  FakePackFiles slash;
  slash.put(builtin / "tool" / "pack.toml", manifest("tool"));
  slash.put(builtin / "tool" / "clean.toml", rule("other/cache", R"(%TEMP%\x)"));
  expect_error(load(slash, "tool"), "case 'other/cache': field 'id' must hold only");

  FakePackFiles files;
  put_pack(files, builtin, "tool");
  files.put(builtin / "tool" / "attribution.toml",
            area("a", R"(%TEMP%\x)", R"({ kind = "clean-rule", ref = "missing" })"));
  expect_error(load(files, "tool"), "area 'tool/a': field 'reclaim.ref' names no clean case");

  files.put(builtin / "tool" / "attribution.toml",
            area("a", R"(%TEMP%\x)", R"({ kind = "clean-rule", ref = "other/cache" })"));
  expect_error(load(files, "tool"), "names no clean case of this pack: 'other/cache'");

  files.put(builtin / "tool" / "attribution.toml",
            area("a", R"(%TEMP%\x)", R"({ kind = "relocate", ref = "other/home" })"));
  expect_error(load(files, "tool"), "names a case of another pack");
}

TEST(LoadPack, RejectsJobCapsInClasses) {
  FakePackFiles files;
  files.put(builtin / "tool" / "pack.toml", manifest("tool"));
  files.put(builtin / "tool" / "classes.toml",
            "[[match]]\nid = \"b\"\nclass = \"build\"\nimage = [\"x.exe\"]\n"
            "[caps.build]\ncpu_percent = 50\n");
  expect_error(load(files, "tool"), "field 'caps' is not allowed in a pack");
}

TEST(LoadPack, RefusesAPackForANewerMyClean) {
  FakePackFiles files;
  put_pack(files, builtin, "tool");
  files.put(builtin / "tool" / "pack.toml", manifest("tool", "0.2.0"));
  const Result<Pack> pack = load(files, "tool");
  expect_error(pack, "needs MyClean 0.2.0 or newer; this is 0.1.0");
  EXPECT_EQ(pack.error().code, Errc::refused);
}

TEST(LoadPack, RejectsTwoCasesClaimingTheSameThing) {
  FakePackFiles files;
  files.put(builtin / "tool" / "pack.toml", manifest("tool"));
  files.put(builtin / "tool" / "clean.toml",
            rule("one", R"(%TEMP%\Tool\*)") + rule("two", R"(%temp%\tool\*)"));
  expect_error(load(files, "tool"),
               R"(case 'tool/two' claims clean target '%temp%\tool\*', as case 'tool/one' does)");
}

TEST(LoadPacks, ARejectedPackDoesNotStopTheOthers) {
  FakePackFiles files;
  put_pack(files, builtin, "alpha", R"(%TEMP%\alpha\*)");
  files.put(builtin / "broken" / "pack.toml", "id = ");
  put_pack(files, builtin, "gamma", R"(%TEMP%\gamma\*)");

  const LoadReport report = load_packs(files, builtin, std::nullopt, running);
  ASSERT_EQ(report.packs.size(), 2U);
  EXPECT_EQ(report.packs[0].manifest.id, "alpha");
  EXPECT_EQ(report.packs[1].manifest.id, "gamma");
  ASSERT_EQ(report.errors.size(), 1U);
  EXPECT_NE(report.errors[0].message.find("pack 'broken': "), std::string::npos);
  EXPECT_NE(report.errors[0].message.find("is not valid TOML"), std::string::npos);
  ASSERT_EQ(report.outcomes.size(), 3U);
  EXPECT_EQ(report.outcomes[1].status, PackStatus::rejected);
  EXPECT_EQ(report.outcomes[1].message, report.errors[0].message);
}

TEST(LoadPacks, OwnerPacksShadowBuiltInOnesAndComeFirst) {
  FakePackFiles files;
  put_pack(files, builtin, "alpha", R"(%TEMP%\builtin\*)");
  put_pack(files, builtin, "beta", R"(%TEMP%\beta\*)");
  put_pack(files, owner, "alpha", R"(%TEMP%\owner\*)");

  const LoadReport report = load_packs(files, builtin, owner, running);
  EXPECT_TRUE(report.errors.empty());
  ASSERT_EQ(report.packs.size(), 2U);
  EXPECT_EQ(report.packs[0].origin, Origin::owner);
  EXPECT_EQ(report.packs[0].clean.at(0).targets.at(0), R"(%TEMP%\owner\*)");
  EXPECT_EQ(report.packs[1].manifest.id, "beta");
  ASSERT_EQ(report.outcomes.size(), 3U);
  EXPECT_EQ(report.outcomes[1].origin, Origin::builtin);
  EXPECT_EQ(report.outcomes[1].status, PackStatus::shadowed);
}

TEST(LoadPacks, ARejectedOwnerPackLeavesTheBuiltInOneInEffect) {
  FakePackFiles files;
  put_pack(files, builtin, "alpha");
  files.put(owner / "alpha" / "pack.toml", manifest("alpha"));  // no content file

  const LoadReport report = load_packs(files, builtin, owner, running);
  ASSERT_EQ(report.packs.size(), 1U);
  EXPECT_EQ(report.packs[0].origin, Origin::builtin);
  EXPECT_EQ(report.outcomes.at(0).status, PackStatus::rejected);
  EXPECT_EQ(report.errors.size(), 1U);
}

TEST(LoadPacks, TheOwnersSettingsDisablePacks) {
  FakePackFiles files;
  put_pack(files, builtin, "alpha", R"(%TEMP%\alpha\*)");
  put_pack(files, builtin, "beta", R"(%TEMP%\beta\*)");
  files.put(owner / "packs.toml", R"(disabled = ["alpha"])");

  const LoadReport report = load_packs(files, builtin, owner, running);
  EXPECT_TRUE(report.errors.empty());
  ASSERT_EQ(report.packs.size(), 1U);
  EXPECT_EQ(report.packs[0].manifest.id, "beta");
  EXPECT_EQ(report.outcomes.at(0).status, PackStatus::disabled);

  files.put(owner / "packs.toml", R"(enabled = ["alpha"])");
  const LoadReport invalid = load_packs(files, builtin, owner, running);
  ASSERT_EQ(invalid.errors.size(), 1U);
  EXPECT_NE(invalid.errors[0].message.find("packs.toml: field 'enabled' is not a known field"),
            std::string::npos);
  EXPECT_EQ(invalid.packs.size(), 2U);
}

TEST(LoadPacks, ConflictsRejectTheLaterPackNamingBothCases) {
  FakePackFiles files;
  put_pack(files, builtin, "alpha", R"(%TEMP%\shared\*)");
  put_pack(files, builtin, "beta", R"(%temp%\SHARED\*)");

  const LoadReport report = load_packs(files, builtin, std::nullopt, running);
  ASSERT_EQ(report.packs.size(), 1U);
  EXPECT_EQ(report.packs[0].manifest.id, "alpha");
  ASSERT_EQ(report.errors.size(), 1U);
  EXPECT_EQ(report.errors[0].message,
            R"(pack 'beta': case 'beta/cache' claims clean target '%temp%\SHARED\*', already )"
            R"(claimed by case 'alpha/cache')");
  EXPECT_EQ(report.outcomes.at(1).status, PackStatus::rejected);
}

TEST(LoadPacks, AnOwnerPackWinsAConflictWithABuiltInPack) {
  FakePackFiles files;
  put_pack(files, builtin, "alpha", R"(%TEMP%\shared\*)");
  put_pack(files, owner, "mine", R"(%TEMP%\shared\*)");

  const LoadReport report = load_packs(files, builtin, owner, running);
  ASSERT_EQ(report.packs.size(), 1U);
  EXPECT_EQ(report.packs[0].manifest.id, "mine");
  EXPECT_EQ(report.outcomes.at(1).status, PackStatus::rejected);
}

TEST(LoadPacks, AMissingOwnerFolderIsNotAnErrorButAMissingBuiltInOneIs) {
  FakePackFiles files;
  put_pack(files, builtin, "alpha");
  EXPECT_TRUE(load_packs(files, builtin, owner, running).errors.empty());

  const LoadReport report = load_packs(files, R"(C:\elsewhere)", std::nullopt, running);
  ASSERT_EQ(report.errors.size(), 1U);
  EXPECT_EQ(report.errors[0].code, Errc::not_found);
}

TEST(LoadPacks, ClaimsCoverEveryKind) {
  FakePackFiles files;
  files.put(builtin / "alpha" / "pack.toml", manifest("alpha"));
  files.put(builtin / "alpha" / "relocate.toml", std::string(recipe));
  files.put(builtin / "beta" / "pack.toml", manifest("beta"));
  files.put(builtin / "beta" / "relocate.toml", std::string(recipe));
  files.put(builtin / "gamma" / "pack.toml", manifest("gamma"));
  files.put(builtin / "gamma" / "attribution.toml",
            area("a", R"(%TEMP%\x)", R"({ kind = "none" })"));
  files.put(builtin / "delta" / "pack.toml", manifest("delta"));
  files.put(builtin / "delta" / "attribution.toml",
            area("b", R"(%temp%\X)", R"({ kind = "none" })"));

  const LoadReport report = load_packs(files, builtin, std::nullopt, running);
  ASSERT_EQ(report.errors.size(), 2U);
  EXPECT_NE(report.errors[0].message.find("claims relocation setting env 'TOOL_HOME', already "
                                          "claimed by case 'alpha/home'"),
            std::string::npos)
      << report.errors[0].message;
  EXPECT_NE(report.errors[1].message.find("claims attribution path '%TEMP%\\x', already claimed "
                                          "by case 'delta/b'"),
            std::string::npos)
      << report.errors[1].message;
}

}  // namespace
}  // namespace myclean::packs
