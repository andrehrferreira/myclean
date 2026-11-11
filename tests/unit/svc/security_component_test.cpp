#include "svc/security_component.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/events/connections.hpp"
#include "core/events/event_store.hpp"
#include "core/events/events.hpp"
#include "core/ipc/security.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/security/extensions.hpp"
#include "core/security/network.hpp"
#include "core/security/persistence.hpp"
#include "core/security/verdicts.hpp"
#include "core/space/duplicates.hpp"
#include "core/startup/startup.hpp"
#include "core/store/migrations.hpp"
#include "core/telemetry/process_collector.hpp"
#include "core/trust/trust.hpp"
#include "fakes/fake_clock.hpp"
#include "phase1_fakes.hpp"

namespace myclean::svc {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

const WallTime noon = std::chrono::sys_days{std::chrono::November / 10 / 2026} + 12h;

const std::filesystem::path temp_updater = R"(C:\Users\owner\AppData\Local\Temp\upd.exe)";
const std::filesystem::path known_bad = R"(C:\Users\owner\AppData\Roaming\x\agent.exe)";
const std::filesystem::path tool = R"(C:\Program Files\Tool\tool.exe)";

// Counts every write, so a test can show that the scan made none.
class FakeStartup final : public startup::IStartupSystem {
 public:
  [[nodiscard]] Result<startup::Snapshot> read() const override {
    return startup::Snapshot{
        .run = {{.name = "Updater", .command = '"' + temp_updater.string() + "\" /silent"},
                {.name = "Agent", .command = known_bad.string()},
                {.scope = startup::Scope::machine, .name = "Tool", .command = tool.string()}}};
  }
  [[nodiscard]] Result<void> set_approved(startup::Scope /*scope*/, startup::ApprovedKey /*key*/,
                                          std::string_view /*name*/,
                                          const startup::ApprovedValue& /*value*/) override {
    ++*writes;
    return {};
  }
  [[nodiscard]] Result<void> set_task_enabled(std::string_view /*path*/,
                                              bool /*enabled*/) override {
    ++*writes;
    return {};
  }
  [[nodiscard]] Result<void> set_service_start(std::string_view /*name*/,
                                               const startup::ServiceStart& /*start*/) override {
    ++*writes;
    return {};
  }

  int* writes = nullptr;
};

class FakePersistence final : public security::IPersistenceSystem {
 public:
  [[nodiscard]] security::PersistenceSnapshot read() const override {
    return security::PersistenceSnapshot{
        .winlogon = {{.name = "Shell", .value = "explorer.exe"},
                     {.name = "Userinit", .value = R"(C:\Windows\system32\userinit.exe,)"}},
        .appinit = {{.dlls = ""}},
        .system_folder = R"(C:\Windows\system32)"};
  }
};

class FakeBrowsers final : public security::IBrowserProfiles {
 public:
  [[nodiscard]] security::BrowserSnapshot read() const override {
    return security::BrowserSnapshot{
        .chromium = {{.browser = security::Browser::edge,
                      .profile = "Default",
                      .secure_preferences = R"({"extensions": {"settings": {
                        "corpcorpcorpcorpcorpcorpcorpcorp": {"location": 1, "manifest":
                          {"name": "Corp", "version": "1"}}}}})"}},
        .forced = {{security::Browser::edge, "corpcorpcorpcorpcorpcorpcorpcorp"}}};
  }
};

// Every target named by the fake startup items exists, and nothing else.
class FakePaths final : public security::IPathResolver {
 public:
  [[nodiscard]] std::optional<std::filesystem::path> resolve(
      std::string_view target) const override {
    const std::filesystem::path path{target};
    for (const std::filesystem::path& known : {temp_updater, known_bad, tool}) {
      if (path == known) {
        return known;
      }
    }
    return std::nullopt;
  }
};

class FakeDefender final : public security::IDefender {
 public:
  [[nodiscard]] Result<std::vector<security::Detection>> detections() const override {
    ++*reads;
    return std::vector<security::Detection>{{.threat = "Trojan:Win32/Agent",
                                             .resources = {"file:_" + known_bad.string()},
                                             .time = noon - 24h}};
  }
  [[nodiscard]] Result<security::FileScan> scan_file(const std::filesystem::path& file,
                                                     const std::stop_token& /*stop*/) override {
    scanned->push_back(file);
    return security::FileScan{.detected = true, .threat = "Virus:DOS/EICAR_Test_File"};
  }

  int* reads = nullptr;
  std::vector<std::filesystem::path>* scanned = nullptr;
};

class FakeStamps final : public trust::IImageInspector {
 public:
  [[nodiscard]] Result<trust::FileStamp> stamp(
      const std::filesystem::path& /*image*/) const override {
    return trust::FileStamp{.size = 100, .modified = noon - 48h};
  }
  [[nodiscard]] Result<trust::Signature> verify(
      const std::filesystem::path& /*image*/) const override {
    return trust::Signature{};
  }
};

class FakeHasher final : public space::IContentHasher {
 public:
  [[nodiscard]] Result<space::Digest> hash(const std::filesystem::path& /*file*/,
                                           std::optional<Bytes> /*limit*/) const override {
    space::Digest digest{};
    digest[0] = 0xAB;
    return digest;
  }
};

class FakeVirusTotal final : public security::IHashLookup {
 public:
  [[nodiscard]] Result<std::optional<security::HashReport>> lookup(
      std::string_view sha256, std::string_view api_key) override {
    hashes->emplace_back(sha256);
    keys->emplace_back(api_key);
    return std::optional<security::HashReport>{security::HashReport{.malicious = 5, .engines = 70}};
  }

  std::vector<std::string>* hashes = nullptr;
  std::vector<std::string>* keys = nullptr;
};

class FakeResolver final : public security::IResolver {
 public:
  [[nodiscard]] std::optional<std::string> name(std::string_view address) override {
    ++*calls;
    return address == "203.0.113.9" ? std::optional<std::string>("beacon.example.test")
                                    : std::nullopt;
  }

  int* calls = nullptr;
};

class FakeConnections final : public events::IConnectionTable {
 public:
  [[nodiscard]] Result<std::vector<events::Connection>> snapshot() override {
    events::IpAddress remote;
    remote.bytes = {203, 0, 113, 9};
    events::IpAddress silent;
    silent.bytes = {198, 51, 100, 1};
    return std::vector<events::Connection>{{.remote_address = remote,
                                            .remote_port = 443,
                                            .state = events::TcpState::established,
                                            .pid = 10},
                                           {.remote_address = silent,
                                            .remote_port = 80,
                                            .state = events::TcpState::established,
                                            .pid = 20}};
  }
};

// The component with the events tables it reads net_minute from, as in the service.
class WithEventTables final : public IComponent {
 public:
  explicit WithEventTables(SecurityComponent& inner) : inner_(&inner) {}

  [[nodiscard]] std::string_view name() const override { return inner_->name(); }
  [[nodiscard]] std::vector<store::MigrationSet> migrations() const override {
    return {events::event_migrations(), security::security_migrations()};
  }
  [[nodiscard]] Result<void> start(const Context& context) override {
    return inner_->start(context);
  }
  void stop() override { inner_->stop(); }

 private:
  SecurityComponent* inner_;
};

class SecurityComponentTest : public ::testing::Test {
 protected:
  void start(std::string settings = {}) {
    settings_ = std::move(settings);
    auto defender = std::make_unique<FakeDefender>();
    defender->reads = &defender_reads_;
    defender->scanned = &scanned_;
    auto virustotal = std::make_unique<FakeVirusTotal>();
    virustotal->hashes = &hashes_;
    virustotal->keys = &keys_;
    auto resolver = std::make_unique<FakeResolver>();
    resolver->calls = &dns_calls_;
    component_ = std::make_unique<SecurityComponent>(SecuritySources{
        .startup = [this](std::string_view) -> std::unique_ptr<startup::IStartupSystem> {
          auto system = std::make_unique<FakeStartup>();
          system->writes = &startup_writes_;
          return system;
        },
        .persistence = [](std::string_view) -> std::unique_ptr<security::IPersistenceSystem> {
          return std::make_unique<FakePersistence>();
        },
        .browsers = [](std::string_view) -> std::unique_ptr<security::IBrowserProfiles> {
          return std::make_unique<FakeBrowsers>();
        },
        .paths = [](std::string_view) -> std::unique_ptr<security::IPathResolver> {
          return std::make_unique<FakePaths>();
        },
        .defender = std::move(defender),
        .stamps = std::make_unique<FakeStamps>(),
        .hasher = std::make_unique<FakeHasher>(),
        .virustotal = std::move(virustotal),
        .resolver = std::move(resolver),
        .connections = std::make_unique<FakeConnections>(),
        .settings = [this]() -> Result<std::optional<std::string>> {
          return settings_.empty() ? std::optional<std::string>{} : std::optional(settings_);
        },
        .image_trust = [](const std::filesystem::path& image) -> Result<trust::TrustSignal> {
          if (image == tool) {
            return trust::assess(image,
                                 {.status = trust::SignatureStatus::valid, .signer = "Tool Inc"},
                                 trust::PathClass::protected_location,
                                 std::vector<rules::KbEntry>{{.id = "tool", .image = "tool.exe"}});
          }
          return trust::assess(image, trust::Signature{}, trust::PathClass::user_writable, {});
        },
        .process_trust = [](std::uint32_t, WallTime) -> Result<trust::TrustSignal> {
          return fail(Errc::access_denied, "not in this test");
        },
        .view = [this] { return view_; },
        .period = std::chrono::milliseconds::zero()});
    wrapper_ = std::make_unique<WithEventTables>(*component_);
    ASSERT_TRUE(harness_.start(*wrapper_).has_value());
  }

  [[nodiscard]] Json ok(std::string_view method, const Json& params = Json::object()) {
    Result<Json> result = harness_.call(method, params);
    EXPECT_TRUE(result.has_value()) << result.error().message;
    return result.value_or(Json());
  }

  static const Json& item(const Json& scan, std::string_view name) {
    for (const Json& entry : scan.at("items")) {
      if (entry.at("name").get<std::string>() == name) {
        return entry;
      }
    }
    static const Json none;
    ADD_FAILURE() << "no item " << name;
    return none;
  }

  fakes::FakeClock& clock() { return clock_; }
  [[nodiscard]] const testing::ComponentHarness& harness() const { return harness_; }
  SecurityComponent& component() { return *component_; }
  void set_settings(std::string settings) { settings_ = std::move(settings); }
  void set_view(std::shared_ptr<const telemetry::ProcessView> view) { view_ = std::move(view); }
  [[nodiscard]] int startup_writes() const { return startup_writes_; }
  [[nodiscard]] int defender_reads() const { return defender_reads_; }
  [[nodiscard]] int dns_calls() const { return dns_calls_; }
  [[nodiscard]] const std::vector<std::filesystem::path>& scanned() const { return scanned_; }
  [[nodiscard]] const std::vector<std::string>& hashes() const { return hashes_; }
  [[nodiscard]] const std::vector<std::string>& keys() const { return keys_; }

 private:
  fakes::FakeClock clock_{noon};
  testing::ComponentHarness harness_{clock_};
  std::unique_ptr<SecurityComponent> component_;
  std::unique_ptr<WithEventTables> wrapper_;
  std::string settings_;
  int startup_writes_ = 0;
  int defender_reads_ = 0;
  int dns_calls_ = 0;
  std::vector<std::filesystem::path> scanned_;
  std::vector<std::string> hashes_;
  std::vector<std::string> keys_;
  std::shared_ptr<const telemetry::ProcessView> view_;
};
// Spec: "Defender detection produces a verdict", "Unsigned binary without verdict is only
// suspicious", "Default Winlogon values are not flagged" and "Scan leaves the audit log without
// actions".
TEST_F(SecurityComponentTest, ScanJudgesOnlyByDefenderAndStatesReasons) {
  start();
  const Json scan = ok(ipc::security_scan_method);
  const Json& agent = item(scan, "Agent");
  EXPECT_EQ(agent.at("status"), "known_dangerous");
  EXPECT_EQ(agent.at("verdict").at("source"), "Microsoft Defender");
  EXPECT_EQ(agent.at("verdict").at("threat"), "Trojan:Win32/Agent");
  const Json& updater = item(scan, "Updater");
  EXPECT_EQ(updater.at("status"), "suspicious");
  EXPECT_EQ(updater.at("summary"),
            "suspicious because unsigned, runs from a user-writable path, not in the knowledge "
            "base");
  EXPECT_EQ(updater.at("verdict"), nullptr);
  EXPECT_EQ(updater.at("startup_item"), "run:user:Updater");
  EXPECT_EQ(updater.at("location"), R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Run)");
  EXPECT_EQ(item(scan, "Tool").at("status"), "ok");
  EXPECT_EQ(item(scan, "Shell").at("status"), "default");
  EXPECT_EQ(item(scan, "Userinit").at("status"), "default");
  EXPECT_EQ(item(scan, "AppInit_DLLs").at("status"), "default");
  EXPECT_EQ(scan.at("counts").at("known_dangerous"), 1);
  EXPECT_EQ(scan.at("counts").at("suspicious"), 1);
  ASSERT_EQ(scan.at("defender").at("detections").size(), 1U);
  EXPECT_EQ(scan.at("virustotal").at("enabled"), false);
  // Read only: no startup change, no Defender scan, and the history read once per 5 minutes.
  static_cast<void>(ok(ipc::security_scan_method));
  EXPECT_EQ(startup_writes(), 0);
  EXPECT_TRUE(scanned().empty());
  EXPECT_EQ(defender_reads(), 1);
  clock().set_wall(noon + 6min);
  static_cast<void>(ok(ipc::security_scan_method));
  EXPECT_EQ(defender_reads(), 2);
}

// Spec: "VirusTotal disabled makes no network call".
TEST_F(SecurityComponentTest, VirusTotalOffMakesNoLookup) {
  start();
  static_cast<void>(ok(ipc::security_scan_method));
  for (int second = 0; second < 60; ++second) {
    clock().set_wall(noon + std::chrono::seconds{second});
    component().tick();
  }
  EXPECT_TRUE(hashes().empty());
}

// Spec: "No API key keeps VirusTotal off".
TEST_F(SecurityComponentTest, VirusTotalWithoutAKeyStaysOff) {
  start("[virustotal]\nenabled = true\n");
  const Json scan = ok(ipc::security_scan_method);
  EXPECT_EQ(scan.at("virustotal").at("enabled"), false);
  EXPECT_NE(scan.at("virustotal").at("note").get<std::string>().find("API key"), std::string::npos);
  component().tick();
  EXPECT_TRUE(hashes().empty());
}

// Spec: "Enabled lookup sends only the hash".
TEST_F(SecurityComponentTest, VirusTotalSendsOnlyTheHashOfSuspiciousFiles) {
  start("[virustotal]\nenabled = true\napi_key = \"k3y\"\n");
  const Json first = ok(ipc::security_scan_method);
  EXPECT_EQ(first.at("virustotal").at("queued"), 1);  // the suspicious updater only
  component().tick();
  component().tick();  // the next lookup waits 15 s
  ASSERT_EQ(hashes().size(), 1U);
  EXPECT_EQ(hashes()[0], "ab" + std::string(62, '0'));
  EXPECT_EQ(keys()[0], "k3y");
  const Json scan = ok(ipc::security_scan_method);
  const Json& updater = item(scan, "Updater");
  EXPECT_EQ(updater.at("status"), "known_dangerous");
  EXPECT_EQ(updater.at("verdict").at("source"), "VirusTotal");
  EXPECT_EQ(updater.at("summary"), "known dangerous: VirusTotal, flagged by 5 of 70 engines");
  EXPECT_EQ(updater.at("virustotal").at("engines"), 70);
}

// Spec: "Scan of a suspicious file reports only".
TEST_F(SecurityComponentTest, DefenderScanOfOneItemBecomesItsVerdict) {
  start();
  const Json scan = ok(ipc::security_scan_method);
  const Json& updater = item(scan, "Updater");
  const Json started = ok(ipc::security_defender_scan_method, {{"item", updater.at("id")}});
  EXPECT_EQ(started.at("started"), true);
  component().wait_for_defender_scan();
  ASSERT_EQ(scanned().size(), 1U);
  EXPECT_EQ(scanned()[0], temp_updater);
  const Json again = ok(ipc::security_scan_method);
  EXPECT_EQ(item(again, "Updater").at("verdict").at("threat"), "Virus:DOS/EICAR_Test_File");
  EXPECT_FALSE(harness().call(ipc::security_defender_scan_method, {{"item", 999}}));
  const Json& shell = item(again, "Shell");
  EXPECT_FALSE(harness().call(ipc::security_defender_scan_method, {{"item", shell.at("id")}}));
  EXPECT_NE(harness().code(ipc::security_defender_scan_method, {{"item", "x"}}), 0);
}

// Spec: "Policy-forced extension is flagged".
TEST_F(SecurityComponentTest, ExtensionsCarryTheirOriginAndFlag) {
  start();
  const Json list = ok(ipc::security_extensions_method);
  ASSERT_EQ(list.at("extensions").size(), 1U);
  EXPECT_EQ(list.at("extensions")[0].at("origin"), "policy");
  EXPECT_EQ(list.at("extensions")[0].at("flag"), "probably not installed by you");
}

// Spec: "Reverse DNS off by default" and "Reverse DNS enabled by the owner".
TEST_F(SecurityComponentTest, NetworkNamesOnlyWhenTheOwnerTurnedThemOn) {
  telemetry::ProcessView view{.time = noon};
  view.rows.push_back(
      {.info = {.pid = 10, .created = noon - 1h, .image = "beacon.exe", .session = 1}});
  view.rows.push_back(
      {.info = {.pid = 20, .created = noon - 1h, .image = "app.exe", .session = 1}});
  set_view(std::make_shared<const telemetry::ProcessView>(std::move(view)));
  start();
  // The tray saw a window of the app only.
  EXPECT_NE(harness().code(ipc::security_windows_method, {{"windows", {{"pids", {20}}}}}), 0);
  static_cast<void>(component().windows(Json{{"windows", {{"pids", {20}}}}}));

  Json network = ok(ipc::security_network_method);
  EXPECT_EQ(network.at("reverse_dns"), false);
  ASSERT_EQ(network.at("processes").size(), 2U);
  for (const Json& process : network.at("processes")) {
    EXPECT_EQ(process.at("remotes")[0].at("name"), nullptr);
    EXPECT_EQ(process.at("visible_window"), process.at("pid") == 20);
  }
  component().tick();
  EXPECT_EQ(dns_calls(), 0);

  set_settings("[network]\nreverse_dns = true\n");
  static_cast<void>(ok(ipc::security_network_method, {{"hours", 2}}));
  component().tick();
  EXPECT_EQ(dns_calls(), 2);
  network = ok(ipc::security_network_method);
  for (const Json& process : network.at("processes")) {
    // Spec: "Unresolvable address is still listed": an empty name.
    EXPECT_EQ(process.at("remotes")[0].at("name"),
              process.at("pid") == 10 ? "beacon.example.test" : "");
  }
  EXPECT_FALSE(harness().call(ipc::security_network_method, {{"hours", 0}}));
  EXPECT_FALSE(harness().call(ipc::security_network_method, {{"hours", 10000}}));
}

}  // namespace
}  // namespace myclean::svc
