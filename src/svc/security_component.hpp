#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/events/connections.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/security/extensions.hpp"
#include "core/security/network.hpp"
#include "core/security/persistence.hpp"
#include "core/security/scan.hpp"
#include "core/security/verdicts.hpp"
#include "core/space/duplicates.hpp"
#include "core/startup/startup.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/process_collector.hpp"
#include "core/trust/trust.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

// The machine the security component reads: Win32 adapters in the service, fakes in tests. The
// factories get the owner's SID and may be empty (that source is then left out).
struct SecuritySources {
  std::function<std::unique_ptr<startup::IStartupSystem>(std::string_view owner_sid)> startup;
  std::function<std::unique_ptr<security::IPersistenceSystem>(std::string_view owner_sid)>
      persistence;
  std::function<std::unique_ptr<security::IBrowserProfiles>(std::string_view owner_sid)> browsers;
  std::function<std::unique_ptr<security::IPathResolver>(std::string_view owner_sid)> paths;
  std::unique_ptr<security::IDefender> defender;   // null: no Defender verdicts
  std::unique_ptr<trust::IImageInspector> stamps;  // size and time of files, for stored verdicts
  std::unique_ptr<space::IContentHasher> hasher;   // SHA-256 for VirusTotal
  std::unique_ptr<security::IHashLookup> virustotal;
  std::unique_ptr<security::IResolver> resolver;  // reverse DNS
  std::unique_ptr<events::IConnectionTable> connections;
  // The text of the owner's security.toml, nullopt when there is none.
  std::function<Result<std::optional<std::string>>()> settings;
  // The trust signals of the trust component.
  std::function<Result<trust::TrustSignal>(const std::filesystem::path& image)> image_trust;
  std::function<Result<trust::TrustSignal>(std::uint32_t pid, WallTime created)> process_trust;
  // The telemetry component's newest process table.
  std::function<std::shared_ptr<const telemetry::ProcessView>()> view;
  // How often the worker resolves names and makes VirusTotal lookups; zero: no thread (tests call
  // tick() themselves).
  std::chrono::milliseconds period{1000};
};

// The intruder scan (security spec) as a service component: registers the methods of
// core/ipc/security.hpp and owns tables visible_window and file_verdict. It reads only: nothing
// here closes a process or removes or disables anything; a Defender file scan the owner asks
// for runs with -DisableRemediation. Its worker resolves queued reverse-DNS names (4 per tick)
// and makes at most one VirusTotal lookup per 15 s, both only when the owner turned them on.
class SecurityComponent final : public IComponent {
 public:
  static constexpr std::chrono::minutes defender_cache{5};
  static constexpr std::chrono::seconds virustotal_spacing{15};
  static constexpr std::chrono::days virustotal_fresh{7};
  static constexpr std::size_t names_per_tick = 4;
  static constexpr std::size_t max_detections = 50;

  explicit SecurityComponent(SecuritySources sources);
  SecurityComponent(const SecurityComponent&) = delete;
  SecurityComponent& operator=(const SecurityComponent&) = delete;
  SecurityComponent(SecurityComponent&&) = delete;
  SecurityComponent& operator=(SecurityComponent&&) = delete;
  ~SecurityComponent() override;

  [[nodiscard]] std::string_view name() const override { return "security"; }
  [[nodiscard]] std::vector<store::MigrationSet> migrations() const override {
    return {security::security_migrations()};
  }
  [[nodiscard]] Result<void> start(const Context& context) override;
  void stop() override;
  [[nodiscard]] ipc::ComponentHealth health() const override;

  // The methods; thread-safe after start().
  [[nodiscard]] Result<nlohmann::json> scan();
  [[nodiscard]] Result<nlohmann::json> extensions() const;
  [[nodiscard]] Result<nlohmann::json> network(const nlohmann::json& params);
  [[nodiscard]] Result<nlohmann::json> defender_scan(const nlohmann::json& params);
  [[nodiscard]] Result<nlohmann::json> windows(const nlohmann::json& params);

  // One pass of the worker: queued reverse-DNS names and a due VirusTotal lookup. Public for
  // tests.
  void tick();
  // Waits for a Defender file scan started by defender_scan to end. For tests.
  void wait_for_defender_scan();

 private:
  struct Stored {
    trust::FileStamp stamp;
    bool detected = false;
    std::string threat;
    std::optional<std::uint32_t> flagged;
    std::optional<std::uint32_t> engines;
    WallTime time;
  };

  [[nodiscard]] security::Settings settings();
  [[nodiscard]] std::vector<security::LiveProcess> live() const;
  [[nodiscard]] Result<std::vector<security::Detection>> detections();
  [[nodiscard]] std::optional<Stored> stored(const std::filesystem::path& image,
                                             std::string_view source) const;
  void store_verdict(const std::filesystem::path& image, std::string_view source,
                     const Stored& verdict) const;
  [[nodiscard]] std::optional<security::Verdict> verdict_of(
      const std::filesystem::path& image, std::span<const security::Detection> known) const;
  // Every persistence point assessed; sources that could not be read go to `problems`.
  [[nodiscard]] std::vector<security::Finding> findings(std::span<const security::Detection> known,
                                                        nlohmann::json& problems) const;
  // The answer's item; images due for a VirusTotal lookup are added to `to_look_up`.
  [[nodiscard]] nlohmann::json item_json(const security::Finding& finding, std::size_t id,
                                         std::vector<std::filesystem::path>& to_look_up) const;
  // Keeps the scan's findings for defender_scan and queues the lookups; the queue's length.
  std::size_t remember(std::vector<security::Finding> found, const security::Settings& options,
                       std::vector<std::filesystem::path> to_look_up);
  [[nodiscard]] std::optional<trust::TrustSignal> talker_trust(
      const security::Talker& talker) const;
  [[nodiscard]] nlohmann::json talker_json(const security::Talker& talker, bool reverse_dns,
                                           WallTime now) const;
  void look_up(const std::filesystem::path& image, const std::string& api_key);
  void note(std::string_view what, const std::optional<std::string>& problem);
  void run(const std::stop_token& stop);

  SecuritySources sources_;
  store::Store* store_ = nullptr;
  const os::IClock* clock_ = nullptr;
  std::unique_ptr<startup::IStartupSystem> startup_;
  std::unique_ptr<security::IPersistenceSystem> persistence_;
  std::unique_ptr<security::IBrowserProfiles> browsers_;
  std::unique_ptr<security::IPathResolver> paths_;
  std::unique_ptr<security::ReverseDns> dns_;
  std::unique_ptr<security::WindowLog> windows_;

  std::mutex scan_mutex_;                  // one scan at a time
  mutable std::mutex mutex_;               // guards the members below
  std::vector<security::Finding> latest_;  // of the latest scan: its ids are positions + 1
  std::optional<std::vector<security::Detection>> detections_;
  std::optional<Error> detections_error_;
  std::optional<WallTime> detections_time_;
  std::deque<std::filesystem::path> lookups_;  // VirusTotal queue
  std::set<std::filesystem::path> queued_;
  std::string api_key_;  // of the latest scan; empty: VirusTotal off
  std::optional<WallTime> next_lookup_;
  std::map<std::string, std::string> problems_;  // by what

  std::atomic<bool> scanning_{false};
  std::jthread defender_thread_;
  std::jthread worker_;
};

}  // namespace myclean::svc
