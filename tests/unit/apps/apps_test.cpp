#include "core/apps/apps.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/settings.hpp"
#include "core/rules/schema.hpp"
#include "placement/placement_fakes.hpp"

namespace myclean::apps {
namespace {

using Json = nlohmann::json;
using placement::RegistryType;
using placement::RegistryValue;
using placement::testing::FakeRegistry;
using std::chrono::days;

WallTime date(int year, unsigned month, unsigned day) {
  return std::chrono::sys_days{std::chrono::year{year} / std::chrono::month{month} /
                               std::chrono::day{day}};
}

const WallTime now = date(2026, 11, 11);

constexpr std::string_view machine64 =
    R"(HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)";
constexpr std::string_view machine32 =
    R"(HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall)";
constexpr std::string_view user = R"(HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)";

RegistryValue text(std::string value) {
  return RegistryValue{.text = std::move(value), .type = RegistryType::string};
}

RegistryValue dword(std::uint32_t value) {
  return RegistryValue{.text = std::to_string(value), .type = RegistryType::dword};
}

class FakeAppSystem final : public IAppSystem {
 public:
  Result<std::vector<AppxPackage>> packages = std::vector<AppxPackage>{};
  Result<std::vector<RunRecord>> assist = std::vector<RunRecord>{};
  Result<std::vector<RunRecord>> prefetched = std::vector<RunRecord>{};
  Result<std::vector<RunRecord>> moderated = std::vector<RunRecord>{};
  Result<std::string> maker = std::string("ASUSTeK COMPUTER INC.");

  [[nodiscard]] Result<std::vector<AppxPackage>> appx() const override { return packages; }
  [[nodiscard]] Result<std::vector<RunRecord>> user_assist() const override { return assist; }
  [[nodiscard]] Result<std::vector<RunRecord>> prefetch() const override { return prefetched; }
  [[nodiscard]] Result<std::vector<RunRecord>> bam() const override { return moderated; }
  [[nodiscard]] Result<std::string> manufacturer() const override { return maker; }
};

void add_app(FakeRegistry& registry, std::string_view root, const std::string& subkey,
             const std::string& name, const std::string& publisher, const std::string& location,
             const std::string& uninstall = {}) {
  const std::string key = std::string(root) + "\\" + subkey;
  registry.put(key, "DisplayName", text(name));
  registry.put(key, "Publisher", text(publisher));
  registry.put(key, "DisplayVersion", text("1.0"));
  registry.put(key, "InstallDate", text("20240302"));
  registry.put(key, "EstimatedSize", dword(2048));
  if (!location.empty()) {
    registry.put(key, "InstallLocation", text(location));
  }
  if (!uninstall.empty()) {
    registry.put(key, "UninstallString", text(uninstall));
  }
}

const App& named(const std::vector<App>& apps, std::string_view name) {
  for (const App& app : apps) {
    if (app.name == name) {
      return app;
    }
  }
  throw std::runtime_error("no app " + std::string(name));
}

// Spec "Win32 and Appx apps appear together" and "App missing from the knowledge base".
TEST(InventoryTest, ListsWin32AndAppxAppsTogether) {
  FakeRegistry registry;
  add_app(registry, machine32, "{GUID-1}", "Old Tool", "Old Corp", R"(C:\Program Files (x86)\Old)",
          R"("C:\Program Files (x86)\Old\unins000.exe")");
  add_app(registry, user, "Editor", "Editor", "Editor Inc",
          R"(C:\Users\owner\AppData\Local\Editor)",
          R"(C:\Users\owner\AppData\Local\Editor\Update.exe --uninstall)");
  registry.put(std::string(machine64) + R"(\Hidden)", "DisplayName", text("Hidden part"));
  registry.put(std::string(machine64) + R"(\Hidden)", "SystemComponent", dword(1));
  registry.put(std::string(machine64) + R"(\Update)", "DisplayName", text("Update of Old"));
  registry.put(std::string(machine64) + R"(\Update)", "ParentKeyName", text("{GUID-1}"));
  FakeAppSystem system;
  system.packages = std::vector<AppxPackage>{
      {.full_name = "Vendor.Notes_2.0.0.0_x64__abc",
       .family_name = "Vendor.Notes_abc",
       .display_name = "Notes",
       .publisher = "Vendor",
       .version = "2.0.0.0",
       .location = R"(C:\Program Files\WindowsApps\Vendor.Notes_2.0.0.0_x64__abc)",
       .installed = date(2025, 1, 5),
       .size = Bytes{5000}}};
  const Facts facts = read_facts(registry, system);
  EXPECT_TRUE(facts.problems.empty());
  const std::vector<App> apps = inventory(facts, ProcessHistory{}, {}, now);
  ASSERT_EQ(apps.size(), 3U);
  EXPECT_EQ(apps[0].name, "Editor");
  EXPECT_EQ(apps[1].name, "Old Tool");
  EXPECT_EQ(apps[1].publisher, "Old Corp");
  EXPECT_EQ(apps[1].version, "1.0");
  EXPECT_EQ(apps[1].install_date, "2024-03-02");
  EXPECT_EQ(apps[1].size, Bytes{std::uint64_t{2048} * 1024});
  EXPECT_EQ(apps[1].description, "unknown");
  EXPECT_EQ(apps[2].kind, Kind::appx);
  EXPECT_EQ(apps[2].install_date, "2025-01-05");
  EXPECT_EQ(apps[2].size, Bytes{5000});
  EXPECT_EQ(apps[2].verdict, Verdict::unknown);
}

// Spec "Newest timestamp wins and names its source".
TEST(LastUsedTest, TheNewestTimestampWinsAndNamesItsSource) {
  const App app{.roots = {R"(C:\Program Files\App)"}};
  const std::array<RunRecord, 3> records = {
      RunRecord{.source = UsageSource::user_assist,
                .path = R"(C:\Program Files\App\app.exe)",
                .last_run = date(2026, 3, 2)},
      RunRecord{.source = UsageSource::prefetch,
                .path = R"(C:\PROGRAM FILES\APP\APP.EXE)",
                .last_run = date(2026, 5, 10)},
      RunRecord{.source = UsageSource::bam,
                .path = R"(C:\Program Files\Application\other.exe)",
                .last_run = date(2026, 9, 1)}};
  const LastUsed used = last_used(app, records, ProcessHistory{}, now);
  EXPECT_EQ(used.time, date(2026, 5, 10));
  EXPECT_EQ(used.source, UsageSource::prefetch);
  EXPECT_EQ(used.confidence, Confidence::high);
}

// Spec "Own history becomes authoritative".
TEST(LastUsedTest, OwnHistoryOverHalfAYearIsHighConfidence) {
  ProcessHistory history;
  history.add(R"(C:\Windows\explorer.exe)", now - days{200});
  history.add(R"(C:\Program Files\App\App.exe)", date(2026, 9, 20));
  const App app{.roots = {R"(C:\Program Files\App)"}};
  const std::array<RunRecord, 1> records = {RunRecord{.source = UsageSource::user_assist,
                                                      .path = R"(C:\Program Files\App\app.exe)",
                                                      .last_run = date(2026, 3, 2)}};
  const LastUsed used = last_used(app, records, history, now);
  EXPECT_EQ(used.time, date(2026, 9, 20));
  EXPECT_EQ(used.source, UsageSource::myclean);
  EXPECT_EQ(used.confidence, Confidence::high);
}

// Spec "No evidence from any source".
TEST(LastUsedTest, NoEvidenceIsNoUsageRecordedWithTheHistorySpan) {
  ProcessHistory history;
  history.add(R"(C:\Windows\explorer.exe)", now - days{40});
  const App app{.name = "Idle", .roots = {R"(C:\Program Files\Idle)"}};
  const LastUsed used = last_used(app, {}, history, now);
  EXPECT_FALSE(used.time);
  EXPECT_EQ(used.confidence, Confidence::low);
  const std::vector<App> apps = {App{.name = "Idle", .last_used = used}};
  const Json json = apps_json(apps, Facts{}, history, std::nullopt, now);
  EXPECT_EQ(json.at("apps")[0].at("last_used").at("text"),
            "no usage recorded (MyClean's process history covers 40 days)");
  EXPECT_EQ(json.at("history").at("days"), 40);
}

TEST(LastUsedTest, AppxRunsMatchTheFamilyName) {
  const App app{.kind = Kind::appx, .family = "Microsoft.WindowsCalculator_8wekyb3d8bbwe"};
  const std::array<RunRecord, 2> records = {
      RunRecord{.source = UsageSource::user_assist,
                .path = "Microsoft.WindowsCalculator_8wekyb3d8bbwe!App",
                .last_run = date(2026, 6, 1)},
      RunRecord{.source = UsageSource::bam,
                .path = "Microsoft.WindowsCalculator_8wekyb3d8bbwe",
                .last_run = date(2026, 7, 1)}};
  EXPECT_EQ(last_used(app, records, ProcessHistory{}, now).time, date(2026, 7, 1));
}

// Spec "Filter keeps only unused apps".
TEST(UnusedTest, TheFilterKeepsUnusedAppsAndAppsWithoutUsage) {
  const std::vector<App> apps = {
      App{.name = "A",
          .last_used = {.time = now - days{200},
                        .source = UsageSource::prefetch,
                        .confidence = Confidence::high}},
      App{.name = "B", .last_used = {.time = now - days{30}, .source = UsageSource::bam}},
      App{.name = "C"}};
  const Json json = apps_json(apps, Facts{}, ProcessHistory{}, 180, now);
  ASSERT_EQ(json.at("apps").size(), 2U);
  EXPECT_EQ(json.at("apps")[0].at("name"), "A");
  EXPECT_EQ(json.at("apps")[0].at("id"), 1);
  EXPECT_EQ(json.at("apps")[0].at("last_used").at("source"), "Prefetch");
  EXPECT_EQ(json.at("apps")[0].at("last_used").at("confidence"), "high");
  EXPECT_EQ(json.at("apps")[1].at("name"), "C");
  EXPECT_EQ(json.at("apps")[1].at("id"), 3);
  EXPECT_EQ(json.at("unused_days"), 180);
}

// Spec "OEM app is kept", "Runtime is kept", "System Appx is kept", "Unknown app is not guessed
// removable".
TEST(ClassifyTest, KeepsOemRuntimesDriversAndSystemAppsAndGuessesNothing) {
  const std::string maker = "ASUSTeK COMPUTER INC.";
  EXPECT_EQ(
      classify(App{.name = "Armoury Crate", .publisher = "ASUS"}, false, maker, nullptr).reason,
      "came with the computer (OEM)");
  const Classification runtime =
      classify(App{.name = "Microsoft Visual C++ 2015-2022 Redistributable (x64) - 14.40.33810",
                   .publisher = "Microsoft Corporation"},
               false, maker, nullptr);
  EXPECT_EQ(runtime.verdict, Verdict::keep);
  EXPECT_EQ(runtime.reason, "runtime or framework");
  EXPECT_EQ(classify(App{.kind = Kind::appx, .name = "Photos"}, true, maker, nullptr).reason,
            "Windows system app");
  EXPECT_EQ(classify(App{.name = "NVIDIA Graphics Driver 555.85"}, false, maker, nullptr).reason,
            "driver");
  EXPECT_EQ(
      classify(App{.name = "Some Tool", .publisher = "Tools Ltd"}, false, maker, nullptr).verdict,
      Verdict::unknown);
  const rules::KbEntry safe{.id = "vendor.tool",
                            .image = "tool.exe",
                            .description = "A tool.",
                            .category = "utility",
                            .safe_to_remove = true};
  EXPECT_EQ(classify(App{.name = "Tool", .publisher = "Tools Ltd"}, false, maker, &safe).verdict,
            Verdict::removable);
}

TEST(ClassifyTest, MatchesTheMakerByItsFirstWord) {
  EXPECT_TRUE(same_maker("ASUSTeK Computer Inc.", "ASUS"));
  EXPECT_TRUE(same_maker("ASUS", "ASUSTeK COMPUTER INC."));
  EXPECT_TRUE(same_maker("Dell Inc", "Dell Inc."));
  EXPECT_FALSE(same_maker("Microsoft Corporation", "ASUS"));
  EXPECT_FALSE(same_maker("System Tools", "System manufacturer"));
  EXPECT_FALSE(same_maker("", "ASUS"));
}

TEST(InventoryTest, TheKnowledgeBaseDescribesAnAppByItsPrograms) {
  FakeRegistry registry;
  add_app(registry, machine64, "Armoury", "Armoury Crate", "ASUSTeK COMPUTER INC.",
          R"(C:\Program Files\ASUS\ArmouryDevice)");
  FakeAppSystem system;
  system.moderated = std::vector<RunRecord>{
      RunRecord{.source = UsageSource::bam,
                .path = R"(C:\Program Files\ASUS\ArmouryDevice\asus_framework.exe)",
                .last_run = now - days{1}}};
  const std::vector<rules::KbEntry> kb = {rules::KbEntry{.id = "armoury-crate.framework",
                                                         .image = "asus_framework.exe",
                                                         .signer = "ASUSTeK COMPUTER INC.",
                                                         .description = "ASUS framework.",
                                                         .category = "oem-utility",
                                                         .safe_to_remove = true}};
  const std::vector<App> apps = inventory(read_facts(registry, system), ProcessHistory{}, kb, now);
  const App& app = named(apps, "Armoury Crate");
  EXPECT_EQ(app.description, "ASUS framework.");
  EXPECT_EQ(app.verdict, Verdict::keep);  // OEM wins over "safe to remove"
  EXPECT_EQ(app.last_used.source, UsageSource::bam);
}

TEST(RootsTest, UsesTheIconOrUninstallerFolderButNeverAnInstallerCache) {
  EXPECT_EQ(roots_of(Win32App{.icon = R"(C:\Program Files\obs-studio\bin\64bit\obs64.exe)"}),
            std::vector<std::filesystem::path>{R"(C:\Program Files\obs-studio\bin\64bit)"});
  const Win32App steam{.uninstall = R"("C:\Program Files (x86)\Steam\uninstall.exe")"};
  EXPECT_EQ(roots_of(steam), std::vector<std::filesystem::path>{R"(C:\Program Files (x86)\Steam)"});
  EXPECT_TRUE(roots_of(Win32App{.icon = R"(C:\ProgramData\Package Cache\{042d}\vcredist_x64.exe)"})
                  .empty());
  EXPECT_TRUE(roots_of(Win32App{.uninstall = "MsiExec.exe /X{GUID}"}).empty());
  EXPECT_TRUE(roots_of(Win32App{.icon = R"(C:\WINDOWS\Installer\{GUID}\icon.ico)"}).empty());
}

TEST(UserAssistTest, DecodesNamesAndTimes) {
  EXPECT_EQ(rot13("{1NP14R77-02R7-4R5Q-O744-2RO1NR5198O7}\\abgrcnq.rkr"),
            "{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\notepad.exe");
  std::vector<std::uint8_t> data(72, 0);
  data[4] = 3;  // runs
  // 2026-11-11T00:00:00Z as a FILETIME.
  const std::uint64_t ticks = 116'444'736'000'000'000ULL + (1'794'355'200ULL * 10'000'000ULL);
  for (std::size_t index = 0; index < 8; ++index) {
    data.at(60 + index) = static_cast<std::uint8_t>(ticks >> (8U * index));
  }
  EXPECT_EQ(user_assist_time(data), now);
  data[4] = 0;
  EXPECT_FALSE(user_assist_time(data));
  EXPECT_FALSE(user_assist_time(std::span(data).first(16)));
}

// A synthetic format-30 Prefetch file of APP.EXE on one volume.
std::vector<std::uint8_t> prefetch_bytes() {
  std::vector<std::uint8_t> data(0x400, 0);
  const auto put32 = [&](std::size_t at, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
      data[at + index] = static_cast<std::uint8_t>(value >> (8U * index));
    }
  };
  const auto put_text = [&](std::size_t at, std::u16string_view text) {
    for (std::size_t index = 0; index < text.size(); ++index) {
      data[at + (2 * index)] = static_cast<std::uint8_t>(text[index] & 0xFFU);
      data[at + (2 * index) + 1] = static_cast<std::uint8_t>(text[index] >> 8U);
    }
  };
  put32(0, 30);
  data[4] = 'S';
  data[5] = 'C';
  data[6] = 'C';
  data[7] = 'A';
  put_text(0x10, u"APP.EXE");
  const std::uint64_t ticks = 116'444'736'000'000'000ULL + (1'794'355'200ULL * 10'000'000ULL);
  for (std::size_t index = 0; index < 8; ++index) {
    data[0x80 + index] = static_cast<std::uint8_t>(ticks >> (8U * index));
  }
  constexpr std::u16string_view volume = u"\\VOLUME{01D9-1A2B3C4D}";
  constexpr std::u16string_view first = u"\\VOLUME{01D9-1A2B3C4D}\\WINDOWS\\SYSTEM32\\NTDLL.DLL";
  constexpr std::u16string_view second = u"\\VOLUME{01D9-1A2B3C4D}\\PROGRAM FILES\\APP\\APP.EXE";
  put32(0x64, 0x100);
  put32(0x68, static_cast<std::uint32_t>((first.size() + second.size() + 2) * 2));
  put_text(0x100, first);
  put_text(0x100 + ((first.size() + 1) * 2), second);
  put32(0x6C, 0x300);
  put32(0x70, 1);
  put32(0x300, 0x60);  // the device path, right after the 96-byte entry
  put32(0x304, static_cast<std::uint32_t>(volume.size()));
  put32(0x310, 0x1A2B3C4D);
  put_text(0x360, volume);
  return data;
}

TEST(PrefetchTest, ReadsTheLastRunAndTheExecutablePath) {
  const std::vector<std::uint8_t> bytes = prefetch_bytes();
  const Result<PrefetchFile> file = parse_prefetch(bytes);
  ASSERT_TRUE(file.has_value()) << file.error().message;
  const PrefetchFile parsed = file.value_or(PrefetchFile{});
  EXPECT_EQ(parsed.executable, "APP.EXE");
  ASSERT_EQ(parsed.runs.size(), 1U);
  EXPECT_EQ(parsed.runs[0], now);
  EXPECT_EQ(parsed.files.size(), 2U);
  const std::map<std::uint32_t, std::string> drives = {{0x1A2B3C4D, "C:"}};
  EXPECT_EQ(prefetch_executable(parsed, drives), R"(C:\PROGRAM FILES\APP\APP.EXE)");
  EXPECT_EQ(prefetch_executable(parsed, {}), "");
  std::vector<std::uint8_t> other = bytes;
  other[0] = 17;
  EXPECT_FALSE(parse_prefetch(other).has_value());
  EXPECT_FALSE(parse_prefetch(std::span(bytes).first(64)).has_value());
}

}  // namespace
}  // namespace myclean::apps
