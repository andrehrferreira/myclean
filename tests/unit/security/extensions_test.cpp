#include "core/security/extensions.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/units.hpp"

namespace myclean::security {
namespace {

// An extensions.settings entry as Chrome writes it (Secure Preferences), reduced.
std::string chromium_settings(const std::string& entries) {
  return R"({"extensions": {"settings": {)" + entries + "}}}";
}

std::string entry(const std::string& id, int location, bool webstore, const std::string& name,
                  const std::string& extra = {}) {
  return R"(")" + id + R"(": {"location": )" + std::to_string(location) + R"(, "from_webstore": )" +
         (webstore ? "true" : "false") +
         R"(, "first_install_time": "13406169600000000", "manifest": {"name": ")" + name +
         R"(", "version": "1.2.3"})" + extra + "}";
}

const Extension* find(const std::vector<Extension>& list, const std::string& id) {
  for (const Extension& extension : list) {
    if (extension.id == id) {
      return &extension;
    }
  }
  return nullptr;
}

TEST(ChromiumTime, IsMicrosecondsSince1601) {
  EXPECT_EQ(chromium_time("11644473600000000"), WallTime{});
  EXPECT_EQ(chromium_time("11644473601000000"), WallTime{std::chrono::seconds{1}});
  EXPECT_FALSE(chromium_time(""));
  EXPECT_FALSE(chromium_time("abc"));
  EXPECT_FALSE(chromium_time("5"));
}

TEST(ForcedId, TakesTheIdBeforeTheUpdateUrl) {
  EXPECT_EQ(forced_id("abcdefghijklmnopabcdefghijklmnop;https://example.test/update.xml"),
            "abcdefghijklmnopabcdefghijklmnop");
  EXPECT_EQ(forced_id(" abc "), "abc");
}

// Spec: "Policy-forced extension is flagged".
TEST(Extensions, PolicyForcedEdgeExtensionIsFlagged) {
  const BrowserSnapshot snapshot{
      .chromium = {{.browser = Browser::edge,
                    .profile = "Default",
                    .secure_preferences = chromium_settings(
                        entry("forcedforcedforcedforcedforcedfo", 1, false, "Corp Helper"))}},
      .forced = {{Browser::edge, "forcedforcedforcedforcedforcedfo"}}};
  std::vector<std::string> problems;
  const std::vector<Extension> list = extensions(snapshot, problems);
  ASSERT_EQ(list.size(), 1U);
  EXPECT_EQ(list[0].origin, Origin::policy);
  EXPECT_TRUE(list[0].probably_not_by_owner());
  EXPECT_EQ(list[0].browser, Browser::edge);
  EXPECT_EQ(list[0].name, "Corp Helper");
  EXPECT_EQ(list[0].version, "1.2.3");
  EXPECT_TRUE(list[0].installed);
  EXPECT_TRUE(problems.empty());
}

// Spec: "Store extension in a second profile is listed".
TEST(Extensions, StoreExtensionInASecondProfile) {
  const BrowserSnapshot snapshot{
      .chromium = {{.browser = Browser::chrome, .profile = "Default", .secure_preferences = "{}"},
                   {.browser = Browser::chrome,
                    .profile = "Profile 1",
                    .secure_preferences = chromium_settings(
                        entry("storestorestorestorestorestorest", 1, true, "Notes") + "," +
                        entry("componentcomponentcomponentcompo", 5, false, "PDF Viewer") + "," +
                        R"("stubstubstubstubstubstubstubstub": {"active_bit": false})")}}};
  std::vector<std::string> problems;
  const std::vector<Extension> list = extensions(snapshot, problems);
  ASSERT_EQ(list.size(), 1U);  // components of the browser and stubs are not extensions
  EXPECT_EQ(list[0].profile, "Profile 1");
  EXPECT_EQ(list[0].origin, Origin::store);
  EXPECT_FALSE(list[0].probably_not_by_owner());
  EXPECT_TRUE(list[0].enabled);
}

// Spec: "Externally installed extension is flagged".
TEST(Extensions, ExternalBraveExtensionIsFlagged) {
  const BrowserSnapshot snapshot{
      .chromium = {{.browser = Browser::brave,
                    .profile = "Default",
                    .preferences = chromium_settings(
                        entry("registryregistryregistryregistry", 3, false, "Toolbar") + "," +
                        entry("unpackedunpackedunpackedunpacked", 4, false, "Dev") + "," +
                        entry("crxfilecrxfilecrxfilecrxfilecrxf", 1, false, "Sideloaded",
                              R"(, "disable_reasons": [1])"))}}};
  std::vector<std::string> problems;
  const std::vector<Extension> list = extensions(snapshot, problems);
  ASSERT_EQ(list.size(), 3U);
  const Extension* toolbar = find(list, "registryregistryregistryregistry");
  ASSERT_NE(toolbar, nullptr);
  EXPECT_EQ(toolbar->origin, Origin::external);
  EXPECT_TRUE(toolbar->probably_not_by_owner());
  const Extension* dev = find(list, "unpackedunpackedunpackedunpacked");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->origin, Origin::unpacked);
  EXPECT_FALSE(dev->probably_not_by_owner());
  const Extension* sideloaded = find(list, "crxfilecrxfilecrxfilecrxfilecrxf");
  ASSERT_NE(sideloaded, nullptr);
  EXPECT_EQ(sideloaded->origin, Origin::external);
  EXPECT_FALSE(sideloaded->enabled);
}

TEST(Extensions, SecurePreferencesOverridePreferences) {
  const BrowserSnapshot snapshot{
      .chromium = {
          {.browser = Browser::chrome,
           .profile = "Default",
           .preferences = chromium_settings(R"("mergedmergedmergedmergedmergedme": {"state": 0})"),
           .secure_preferences =
               chromium_settings(entry("mergedmergedmergedmergedmergedme", 1, true, "Merged"))}}};
  std::vector<std::string> problems;
  const std::vector<Extension> list = extensions(snapshot, problems);
  ASSERT_EQ(list.size(), 1U);
  EXPECT_FALSE(list[0].enabled);  // the state of Preferences, the rest of Secure Preferences
  EXPECT_EQ(list[0].origin, Origin::store);
}

TEST(Extensions, BrokenJsonIsAProblemNotAFailure) {
  const BrowserSnapshot snapshot{
      .chromium = {{.browser = Browser::chrome, .profile = "Default", .preferences = "{oops"}}};
  std::vector<std::string> problems;
  EXPECT_TRUE(extensions(snapshot, problems).empty());
  ASSERT_EQ(problems.size(), 1U);
  EXPECT_NE(problems[0].find("chrome profile Default"), std::string::npos);
}

TEST(Extensions, FirefoxOrigins) {
  const std::string json = R"({"addons": [
    {"id": "amo@x", "type": "extension", "location": "app-profile", "active": true,
     "version": "2.0", "installDate": 1762776000000, "defaultLocale": {"name": "Store One"},
     "installTelemetryInfo": {"source": "amo"}},
    {"id": "policy@x", "type": "extension", "location": "app-profile", "active": true,
     "defaultLocale": {"name": "Policy One"}, "installTelemetryInfo": {"source": "enterprise-policy"}},
    {"id": "foreign@x", "type": "extension", "location": "winreg-app-global", "active": false,
     "foreignInstall": true, "defaultLocale": {"name": "Foreign"}},
    {"id": "temp@x", "type": "extension", "location": "app-temporary", "active": true,
     "defaultLocale": {"name": "Temp"}},
    {"id": "builtin@mozilla.org", "type": "extension", "location": "app-builtin"},
    {"id": "theme@x", "type": "theme", "location": "app-profile"}]})";
  const BrowserSnapshot snapshot{
      .firefox = {{.profile = "abc.default-release", .extensions_json = json}}};
  std::vector<std::string> problems;
  const std::vector<Extension> list = extensions(snapshot, problems);
  ASSERT_EQ(list.size(), 4U);
  const Extension* store = find(list, "amo@x");
  ASSERT_NE(store, nullptr);
  EXPECT_EQ(store->origin, Origin::store);
  EXPECT_EQ(store->name, "Store One");
  EXPECT_EQ(store->installed, WallTime{std::chrono::milliseconds{1762776000000}});
  EXPECT_EQ(find(list, "policy@x")->origin, Origin::policy);
  EXPECT_EQ(find(list, "foreign@x")->origin, Origin::external);
  EXPECT_FALSE(find(list, "foreign@x")->enabled);
  EXPECT_EQ(find(list, "temp@x")->origin, Origin::unpacked);
}

}  // namespace
}  // namespace myclean::security
