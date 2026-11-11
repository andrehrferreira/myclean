#include "core/security/persistence.hpp"

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/startup/startup.hpp"

namespace myclean::security {
namespace {

startup::Item run_item(const std::string& name, std::string command) {
  return startup::Item{.id = startup::item_id(startup::Source::run, startup::Scope::user, name),
                       .source = startup::Source::run,
                       .scope = startup::Scope::user,
                       .name = name,
                       .display_name = name,
                       .command = std::move(command),
                       .enabled = true};
}

startup::Item task_item(const std::string& path, std::string command) {
  return startup::Item{.id = startup::item_id(startup::Source::task, startup::Scope::machine, path),
                       .source = startup::Source::task,
                       .scope = startup::Scope::machine,
                       .name = path,
                       .display_name = path,
                       .command = std::move(command),
                       .enabled = true,
                       .signer = std::nullopt};
}

TEST(TargetOf, FindsTheExecutableOfACommand) {
  const std::string quoted = target_of(R"("C:\Program Files\App\app.exe" --hidden)");
  EXPECT_EQ(quoted, R"(C:\Program Files\App\app.exe)");
  EXPECT_EQ(target_of(R"(C:\Program Files\App\app.exe --hidden)"),
            R"(C:\Program Files\App\app.exe)");
  EXPECT_EQ(target_of(R"(%LOCALAPPDATA%\Temp\x.exe)"), R"(%LOCALAPPDATA%\Temp\x.exe)");
  EXPECT_EQ(target_of("explorer.exe"), "explorer.exe");
  EXPECT_EQ(target_of(R"(\SystemRoot\System32\drivers\x.sys)"),
            R"(%SystemRoot%\System32\drivers\x.sys)");
  EXPECT_EQ(target_of(R"(\??\C:\Tools\y.exe)"), R"(C:\Tools\y.exe)");
  EXPECT_EQ(target_of(""), "");
}

TEST(TargetOf, TakesTheDllOfRundll32) {
  EXPECT_EQ(target_of(R"(rundll32.exe C:\Users\a\AppData\Roaming\x.dll,Start)"),
            R"(C:\Users\a\AppData\Roaming\x.dll)");
  const std::string quoted =
      target_of(R"(C:\Windows\System32\rundll32.exe "C:\Program Files\y.dll",Entry)");
  EXPECT_EQ(quoted, R"(C:\Program Files\y.dll)");
  EXPECT_EQ(target_of("rundll32.exe"), "rundll32.exe");
}

TEST(DefaultUserinit, AcceptsWindowsOwnValue) {
  constexpr std::string_view system = R"(C:\Windows\system32)";
  EXPECT_TRUE(default_userinit(R"(C:\Windows\system32\userinit.exe,)", system));
  EXPECT_TRUE(default_userinit(R"(C:\WINDOWS\System32\userinit.exe)", system));
  EXPECT_TRUE(default_userinit("userinit.exe,", system));
  EXPECT_FALSE(default_userinit(R"(C:\Windows\system32\userinit.exe,C:\x\evil.exe)", system));
  EXPECT_FALSE(default_userinit(R"(D:\other\userinit.exe)", system));
}

// Spec: "All persistence sources are reported".
TEST(PersistencePoints, ListsEverySource) {
  const std::vector<startup::Item> items = {
      run_item("Updater", R"("C:\Users\a\AppData\Local\Temp\upd.exe" /s)"),
      task_item(R"(\Logon helper)", R"(C:\Tools\helper.exe)")};
  const PersistenceSnapshot extra{
      .wmi = {{.filter = "Every15",
               .query = "SELECT * FROM __InstanceModificationEvent WITHIN 900",
               .consumer_class = "CommandLineEventConsumer",
               .consumer = "Runner",
               .command = R"(C:\ProgramData\r.exe -q)"}},
      .ifeo = {{.image = "notepad.exe", .debugger = R"(C:\Tools\np.exe)"}},
      .system_folder = R"(C:\Windows\system32)"};
  const std::vector<Point> points = persistence_points(items, extra);
  ASSERT_EQ(points.size(), 4U);

  EXPECT_EQ(points[0].source, PointSource::run);
  EXPECT_EQ(points[0].location, R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Run)");
  EXPECT_EQ(points[0].target, R"(C:\Users\a\AppData\Local\Temp\upd.exe)");
  EXPECT_EQ(points[0].startup_item, "run:user:Updater");
  EXPECT_FALSE(points[0].concern);

  EXPECT_EQ(points[1].source, PointSource::task);
  EXPECT_EQ(points[1].location, "Task Scheduler");
  EXPECT_EQ(points[1].target, R"(C:\Tools\helper.exe)");

  EXPECT_EQ(points[2].source, PointSource::wmi);
  EXPECT_EQ(points[2].location, R"(root\subscription)");
  EXPECT_EQ(points[2].name, "Every15 -> CommandLineEventConsumer Runner");
  EXPECT_EQ(points[2].target, R"(C:\ProgramData\r.exe)");
  EXPECT_EQ(points[2].concern, Concern::wmi_consumer);
  EXPECT_EQ(points[2].enabled, true);

  EXPECT_EQ(points[3].source, PointSource::ifeo);
  EXPECT_EQ(points[3].name, "notepad.exe");
  EXPECT_EQ(points[3].target, R"(C:\Tools\np.exe)");
  EXPECT_EQ(points[3].concern, Concern::ifeo_debugger);
  EXPECT_FALSE(points[3].startup_item);
}

// Spec: "Default Winlogon values are not flagged".
TEST(PersistencePoints, DefaultWinlogonAndEmptyAppInitAreStandard) {
  const PersistenceSnapshot extra{
      .wmi = {{.filter = "SCM Event Log Filter",
               .consumer_class = "NTEventLogEventConsumer",
               .consumer = "SCM Event Log Consumer"}},
      .winlogon = {{.name = "Shell", .value = "explorer.exe"},
                   {.name = "Userinit", .value = R"(C:\Windows\system32\userinit.exe,)"}},
      .appinit = {{.dlls = ""}, {.wow64 = true, .dlls = " "}},
      .system_folder = R"(C:\Windows\system32)"};
  const std::vector<Point> points = persistence_points({}, extra);
  ASSERT_EQ(points.size(), 5U);
  for (const Point& point : points) {
    EXPECT_TRUE(point.standard) << point.name;
    EXPECT_FALSE(point.concern) << point.name;
  }
  EXPECT_EQ(points[1].location, R"(HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon)");
  EXPECT_EQ(points[4].location,
            R"(HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\Windows)");
}

TEST(PersistencePoints, ChangedWinlogonAndAppInitDllsAreConcerns) {
  const PersistenceSnapshot extra{
      .winlogon = {{.scope = startup::Scope::user, .name = "Shell", .value = R"(C:\x\shell.exe)"},
                   {.name = "Userinit", .value = R"(C:\Windows\system32\userinit.exe,C:\x\u.exe)"}},
      .appinit = {{.load_enabled = true, .dlls = R"(C:\x\a.dll, "C:\Program Files\b.dll")"}},
      .system_folder = R"(C:\Windows\system32)"};
  const std::vector<Point> points = persistence_points({}, extra);
  ASSERT_EQ(points.size(), 4U);
  EXPECT_EQ(points[0].location, R"(HKCU\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon)");
  EXPECT_EQ(points[0].concern, Concern::winlogon_changed);
  EXPECT_EQ(points[0].target, R"(C:\x\shell.exe)");
  EXPECT_EQ(points[1].concern, Concern::winlogon_changed);
  EXPECT_EQ(points[1].target, R"(C:\x\u.exe)");
  EXPECT_EQ(points[2].target, R"(C:\x\a.dll)");
  EXPECT_EQ(points[3].target, R"(C:\Program Files\b.dll)");
  EXPECT_EQ(points[3].concern, Concern::appinit_dll);
  EXPECT_EQ(points[3].enabled, true);
}

TEST(PersistencePoints, ScriptConsumersAndUnboundConsumers) {
  const PersistenceSnapshot extra{
      .wmi = {{.filter = "F",
               .consumer_class = "ActiveScriptEventConsumer",
               .consumer = "S",
               .command = R"(C:\ProgramData\s.vbs)"},
              {.consumer_class = "ActiveScriptEventConsumer",
               .consumer = "Inline",
               .command = R"(CreateObject("WScript.Shell").Run "calc")"},
              {.filter = "Log", .consumer_class = "LogFileEventConsumer", .consumer = "L"}}};
  const std::vector<Point> points = persistence_points({}, extra);
  ASSERT_EQ(points.size(), 3U);
  EXPECT_EQ(points[0].target, R"(C:\ProgramData\s.vbs)");
  EXPECT_EQ(points[1].target, "");
  EXPECT_EQ(points[1].enabled, false);  // no binding: it runs nothing
  EXPECT_EQ(points[1].name, "(no filter) -> ActiveScriptEventConsumer Inline");
  EXPECT_EQ(points[1].concern, Concern::wmi_consumer);
  EXPECT_FALSE(points[2].concern);
  EXPECT_FALSE(points[2].standard);
}

}  // namespace
}  // namespace myclean::security
