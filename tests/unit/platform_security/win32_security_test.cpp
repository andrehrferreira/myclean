#include "platform/security/win32_security.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/security/extensions.hpp"
#include "core/security/persistence.hpp"
#include "core/security/verdicts.hpp"

namespace myclean::platform {
namespace {

// This machine's persistence sources, read only: Winlogon's values always exist, and every WMI
// subscription read names its consumer.
TEST(Win32PersistenceSystem, ReadsThisMachine) {
  const security::PersistenceSnapshot snapshot = Win32PersistenceSystem{}.read();
  EXPECT_FALSE(snapshot.system_folder.empty());
  bool shell = false;
  for (const security::WinlogonValue& value : snapshot.winlogon) {
    shell = shell || value.name == "Shell";
  }
  EXPECT_TRUE(shell);
  for (const security::WmiSubscription& subscription : snapshot.wmi) {
    EXPECT_FALSE(subscription.consumer_class.empty());
  }
  for (const std::string& problem : snapshot.problems) {
    // Without elevation root\subscription may refuse; nothing else may fail.
    EXPECT_NE(problem.find("WMI"), std::string::npos) << problem;
  }
}

TEST(Win32PathResolver, FindsFilesWhereWindowsWould) {
  const Win32PathResolver resolver;
  const std::optional<std::filesystem::path> notepad = resolver.resolve("notepad.exe");
  ASSERT_TRUE(notepad.has_value());
  EXPECT_TRUE(notepad.value_or(std::filesystem::path{}).is_absolute());
  EXPECT_TRUE(resolver.resolve("notepad"));
  EXPECT_TRUE(resolver.resolve(R"(%SystemRoot%\System32\cmd.exe)"));
  EXPECT_FALSE(resolver.resolve(R"(C:\no\such\folder\x.exe)"));
  EXPECT_FALSE(resolver.resolve(""));
}

TEST(Win32BrowserProfiles, ReadsTheProfilesOfThisUser) {
  const security::BrowserSnapshot snapshot = Win32BrowserProfiles{}.read();
  std::vector<std::string> problems = snapshot.problems;
  static_cast<void>(security::extensions(snapshot, problems));
  for (const security::ChromiumProfile& profile : snapshot.chromium) {
    EXPECT_FALSE(profile.profile.empty());
    EXPECT_NE(profile.profile, "System Profile");
  }
  for (const std::string& problem : problems) {
    ADD_FAILURE() << problem;
  }
}

TEST(OwnerFolders, OfThisUser) {
  const Result<OwnerFolders> folders = owner_folders({});
  ASSERT_TRUE(folders) << folders.error().message;
  EXPECT_TRUE(folders->local_appdata.is_absolute());
  EXPECT_TRUE(folders->roaming.is_absolute());
}

// Only Defender's history is read here; no file is ever scanned by a test.
TEST(Win32Defender, ReadsTheDetectionHistoryOrSaysWhyNot) {
  const Result<std::vector<security::Detection>> history = Win32Defender{}.detections();
  if (history) {
    for (const security::Detection& detection : *history) {
      EXPECT_FALSE(detection.threat.empty());
    }
  } else {
    EXPECT_FALSE(history.error().message.empty());
  }
}

TEST(Win32HashLookup, RefusesAnythingButAHexDigestBeforeAnyRequest) {
  Win32HashLookup lookup;
  EXPECT_FALSE(lookup.lookup("not-a-digest", "key"));
  EXPECT_FALSE(lookup.lookup(std::string(64, 'a'), "key with spaces"));
  EXPECT_FALSE(lookup.lookup(std::string(64, 'A'), "key"));
}

}  // namespace
}  // namespace myclean::platform
