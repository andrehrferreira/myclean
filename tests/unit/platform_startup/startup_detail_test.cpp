// The pure pieces of the startup reader and writer: no machine state is read or changed.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/startup/startup.hpp"
#include "platform/startup/win32_startup.hpp"

namespace myclean::platform::startup_detail {
namespace {

using startup::ServiceStart;
using startup::StartType;

// Raw strings that hold double quotes stay outside the assertion macros: MSVC's preprocessor
// misreads them as ordinary strings inside macro arguments.
constexpr std::string_view quoted_with_arguments = R"("C:\x\a.exe" -k foo)";
constexpr std::string_view indented_quoted = R"(  "C:\Program Files\App\svc.exe")";
constexpr std::string_view unterminated = R"("C:\unterminated\a.exe)";
constexpr std::string_view quoted_path = R"("C:\a b\c.exe")";

TEST(StartupDetail, ExecutableOfAQuotedCommandIsTheQuotedPart) {
  EXPECT_EQ(executable_of(quoted_with_arguments), R"(C:\x\a.exe)");
  EXPECT_EQ(executable_of(indented_quoted), R"(C:\Program Files\App\svc.exe)");
  EXPECT_EQ(executable_of(unterminated), R"(C:\unterminated\a.exe)");
}

TEST(StartupDetail, ExecutableOfAnUnquotedCommandEndsAtTheExe) {
  EXPECT_EQ(executable_of(R"(C:\Windows\system32\svchost.exe -k netsvcs -p)"),
            R"(C:\Windows\system32\svchost.exe)");
  EXPECT_EQ(executable_of(R"(C:\Program Files\App\svc.exe --run)"),
            R"(C:\Program Files\App\svc.exe)");
  EXPECT_EQ(executable_of(R"(C:\App\SVC.EXE)"), R"(C:\App\SVC.EXE)");
  // ".exe" inside a folder name does not end the path.
  EXPECT_EQ(executable_of(R"(C:\a.exed\b.exe -x)"), R"(C:\a.exed\b.exe)");
  // No ".exe": the first word.
  EXPECT_EQ(executable_of(R"(C:\tools\run.bat arg)"), R"(C:\tools\run.bat)");
  EXPECT_EQ(executable_of(""), "");
  EXPECT_EQ(executable_of("   "), "");
}

TEST(StartupDetail, ExecutableOfDropsNtPrefixesAndKeepsVariables) {
  EXPECT_EQ(executable_of(R"(\??\C:\x\a.exe -y)"), R"(C:\x\a.exe)");
  EXPECT_EQ(executable_of(R"(\SystemRoot\System32\a.exe)"), R"(%SystemRoot%\System32\a.exe)");
  EXPECT_EQ(executable_of(R"(%SystemRoot%\System32\svchost.exe -k LocalService)"),
            R"(%SystemRoot%\System32\svchost.exe)");
}

TEST(StartupDetail, SvchostIsRecognizedByFileNameOnly) {
  EXPECT_TRUE(is_svchost(R"(C:\Windows\System32\svchost.exe)"));
  EXPECT_TRUE(is_svchost(R"(%SystemRoot%\system32\SVCHOST.EXE)"));
  EXPECT_TRUE(is_svchost("svchost.exe"));
  EXPECT_FALSE(is_svchost(R"(C:\x\notsvchost.exe)"));
  EXPECT_FALSE(is_svchost(R"(C:\svchost.exe\a.exe)"));
}

TEST(StartupDetail, UnquoteAndJoinCommand) {
  EXPECT_EQ(unquote(quoted_path), R"(C:\a b\c.exe)");
  EXPECT_EQ(unquote(R"(C:\a.exe)"), R"(C:\a.exe)");
  EXPECT_EQ(unquote("\""), "\"");
  EXPECT_EQ(join_command(R"(C:\a.exe)", "/b"), R"(C:\a.exe /b)");
  EXPECT_EQ(join_command(R"(C:\a.exe)", ""), R"(C:\a.exe)");
}

TEST(StartupDetail, ExpandProfileReplacesEveryOccurrenceInAnyCase) {
  EXPECT_EQ(expand_profile(R"(%USERPROFILE%\AppData\Startup)", R"(C:\Users\Ana)"),
            R"(C:\Users\Ana\AppData\Startup)");
  EXPECT_EQ(expand_profile(R"(%userprofile%\a;%UserProfile%\b)", R"(D:\u)"), R"(D:\u\a;D:\u\b)");
  EXPECT_EQ(expand_profile(R"(%APPDATA%\x)", R"(C:\Users\Ana)"), R"(%APPDATA%\x)");
}

TEST(StartupDetail, StartTypesMapToTheServiceCodesBothWays) {
  for (const StartType type : {StartType::boot, StartType::system, StartType::automatic,
                               StartType::manual, StartType::disabled}) {
    EXPECT_EQ(start_type_of(start_type_code(type)), type);
  }
  EXPECT_EQ(start_type_code(StartType::automatic), static_cast<std::uint32_t>(SERVICE_AUTO_START));
  EXPECT_EQ(start_type_code(StartType::manual), static_cast<std::uint32_t>(SERVICE_DEMAND_START));
  EXPECT_EQ(start_type_of(SERVICE_DISABLED), StartType::disabled);
  EXPECT_EQ(start_type_of(7), std::nullopt);
}

TEST(StartupDetail, OnlyAnAutomaticServiceIsWrittenDelayed) {
  EXPECT_TRUE(delayed_flag(ServiceStart{.type = StartType::automatic, .delayed = true}));
  EXPECT_FALSE(delayed_flag(ServiceStart{.type = StartType::automatic, .delayed = false}));
  EXPECT_FALSE(delayed_flag(ServiceStart{.type = StartType::manual, .delayed = true}));
  EXPECT_FALSE(delayed_flag(ServiceStart{.type = StartType::disabled, .delayed = true}));
}

}  // namespace
}  // namespace myclean::platform::startup_detail
