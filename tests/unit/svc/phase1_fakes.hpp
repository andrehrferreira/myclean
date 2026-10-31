#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/alerts/engine.hpp"
#include "core/health/analysis.hpp"
#include "core/health/reading.hpp"
#include "core/health/source.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/rules/data_files.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "core/trust/trust.hpp"
#include "events/event_fakes.hpp"
#include "fake_machine.hpp"
#include "platform/ipc/security.hpp"
#include "svc/alerts_component.hpp"
#include "svc/component.hpp"
#include "svc/disk_health_component.hpp"
#include "svc/events_adapter.hpp"
#include "svc/input_component.hpp"
#include "svc/service.hpp"
#include "svc/telemetry_component.hpp"
#include "svc/trust_component.hpp"
#include "svc_test_support.hpp"

// Fakes and a harness for the phase-1 service components (alerts, trust, disk health, events,
// input), and a whole console-mode service over them.
namespace myclean::svc::testing {

// The shipped data folder (alerts.toml, drive-specs.json, kb/*.json).
inline const std::filesystem::path shipped_data{MYCLEAN_DATA_DIR};

class FakeImageInspector final : public trust::IImageInspector {
 public:
  [[nodiscard]] Result<trust::FileStamp> stamp(const std::filesystem::path& image) const override {
    if (!signatures.contains(image.native())) {
      return fail(Errc::not_found, "no such file");
    }
    return trust::FileStamp{.size = 1, .modified = WallTime{}};
  }
  [[nodiscard]] Result<trust::Signature> verify(const std::filesystem::path& image) const override {
    const std::scoped_lock lock(mutex);
    ++verifications;
    return signatures.at(image.native());
  }

  // Every image that exists, with its signature. Set before the component starts.
  std::map<std::wstring, trust::Signature> signatures;
  mutable std::mutex mutex;
  mutable int verifications = 0;
};

class FakeProcessImages final : public trust::IProcessImages {
 public:
  [[nodiscard]] Result<std::filesystem::path> image_path(std::uint32_t pid,
                                                         WallTime /*created*/) const override {
    const auto found = paths.find(pid);
    if (found == paths.end()) {
      return fail(Errc::access_denied, "process " + std::to_string(pid) + " cannot be opened");
    }
    return found->second;
  }

  std::map<std::uint32_t, std::filesystem::path> paths;
};

class FakeDiskHealthSource final : public health::IDiskHealthSource {
 public:
  [[nodiscard]] Result<std::vector<health::DriveProbe>> probe() const override {
    if (failure) {
      return tl::unexpected(*failure);
    }
    return probes;
  }

  std::vector<health::DriveProbe> probes;
  std::optional<Error> failure;
};

// Keeps the facts it receives. Thread-safe.
class RecordingAlertSink final : public health::IAlertSink {
 public:
  void report(std::span<const alerts::Reading> readings) override {
    const std::scoped_lock lock(mutex_);
    facts_.insert(facts_.end(), readings.begin(), readings.end());
  }

  [[nodiscard]] std::vector<alerts::Reading> facts() const {
    const std::scoped_lock lock(mutex_);
    return facts_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<alerts::Reading> facts_;
};

// A 512-byte NVMe health log: 40 °C, 100 % spare (threshold 10 %), the given counters.
inline std::vector<std::uint8_t> nvme_log(std::int64_t media_errors, std::int64_t bytes_written) {
  constexpr std::int64_t data_unit = 512'000;
  std::vector<std::uint8_t> log(512, 0);
  const auto put = [&](std::size_t offset, std::uint64_t value, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
      log.at(offset + index) = static_cast<std::uint8_t>(value >> (8 * index));
    }
  };
  put(1, 313, 2);
  log.at(3) = 100;
  log.at(4) = 10;
  put(48, static_cast<std::uint64_t>(bytes_written / data_unit), 8);
  put(160, static_cast<std::uint64_t>(media_errors), 8);
  return log;
}

// Disk 2 (volume C: of the fake machine): a Samsung 980 PRO, listed in drive-specs.json.
inline health::DriveProbe samsung(std::int64_t media_errors = 0) {
  return health::DriveProbe{.disk_number = 2,
                            .model = "Samsung SSD 980 PRO 2TB",
                            .serial = "S1",
                            .bus = health::Bus::nvme,
                            .nvme_health_log = nvme_log(media_errors, 300'000'000'000'000)};
}

// Disk 7: a USB enclosure that blocks pass-through.
inline health::DriveProbe usb_blocked() {
  return health::DriveProbe{.disk_number = 7,
                            .model = "ROG ESD-S1C",
                            .serial = "U1",
                            .bus = health::Bus::usb,
                            .problem = "not readable over USB: the bridge rejects pass-through"};
}

// Waits, in real time, until `done` holds or 10 s passed; for the components' own threads.
inline bool eventually(const std::function<bool()>& done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  return true;
}

// One component on a store in a temporary folder, with its methods on a dispatcher.
class ComponentHarness {
 public:
  explicit ComponentHarness(const os::IClock& clock) : clock_(&clock) {}

  // Opens the store with the component's tables and starts the component with `data_dir`.
  [[nodiscard]] Result<void> start(IComponent& component,
                                   const std::filesystem::path& data_dir = shipped_data) {
    const std::vector<store::MigrationSet> sets = component.migrations();
    Result<std::unique_ptr<store::Store>> opened =
        store::Store::open(dir_.path() / "myclean.db", *clock_, sets);
    if (!opened) {
      return tl::unexpected(opened.error());
    }
    store_ = std::move(*opened);
    return component.start(Context(*store_, methods_, *clock_, data_dir));
  }

  [[nodiscard]] Result<nlohmann::json> call(
      std::string_view method, const nlohmann::json& params = nlohmann::json::object()) const {
    return ipc::parse_response(methods_.handle(ipc::make_request(7, method, params)), 7);
  }
  // The JSON-RPC error code of a call; 0 when it succeeded.
  [[nodiscard]] int code(std::string_view method, const nlohmann::json& params) const {
    const nlohmann::json response =
        nlohmann::json::parse(methods_.handle(ipc::make_request(7, method, params)));
    return response.contains("error") ? response.at("error").at("code").get<int>() : 0;
  }

  [[nodiscard]] store::Store& store() { return *store_; }
  // A data folder of the test's own, for files the test writes.
  [[nodiscard]] std::filesystem::path data_dir() const { return dir_.path() / "data"; }
  [[nodiscard]] const std::filesystem::path& dir() const { return dir_.path(); }

 private:
  TempDir dir_;
  const os::IClock* clock_;
  std::unique_ptr<store::Store> store_;
  ipc::Dispatcher methods_;
};

// The fakes behind a whole phase-1 service: the fake machine (with pid 100 renamed
// PowerToys.exe), PowerToys signed by Microsoft in Program Files (a knowledge-base entry),
// editor.exe unsigned in the owner's AppData (unrecognized), pid 300 unreadable; disk 2 a
// healthy Samsung NVMe and disk 7 a USB drive that cannot be read; ETW not available.
struct Phase1Fakes {
  FakeMachine machine = fake_machine();
  FakeImageInspector* inspector = nullptr;
  FakeProcessImages* images = nullptr;
  FakeDiskHealthSource* disks = nullptr;
  events::testing::FakeEventSource* events = nullptr;
  rules::DiskDataFiles files;
};

inline const std::wstring powertoys_path = LR"(C:\Program Files\PowerToys\PowerToys.exe)";
inline const std::wstring editor_path = LR"(C:\Users\owner\AppData\Local\Editor\editor.exe)";

// The components of the service, in the start order of myclean-svc.
inline std::vector<std::unique_ptr<IComponent>> phase1_components(Phase1Fakes& fakes) {
  fakes.machine.processes->processes()[1].image = "PowerToys.exe";
  auto inspector = std::make_unique<FakeImageInspector>();
  inspector->signatures = {
      {powertoys_path, trust::Signature{.status = trust::SignatureStatus::valid,
                                        .signer = "Microsoft Corporation"}},
      {editor_path, trust::Signature{.status = trust::SignatureStatus::none, .signer = {}}}};
  auto images = std::make_unique<FakeProcessImages>();
  images->paths = {{100, powertoys_path}, {101, powertoys_path}, {200, editor_path}};
  auto disks = std::make_unique<FakeDiskHealthSource>();
  disks->probes = {samsung(), usb_blocked()};
  auto event_source = std::make_unique<events::testing::FakeEventSource>();
  event_source->fail_with(Error{.code = Errc::access_denied, .message = "not elevated"});
  fakes.inspector = inspector.get();
  fakes.images = images.get();
  fakes.disks = disks.get();
  fakes.events = event_source.get();

  auto alerts = std::make_unique<AlertsComponent>(fakes.files);
  auto trust = std::make_unique<TrustComponent>(
      TrustSources{.inspector = std::move(inspector),
                   .images = std::move(images),
                   .user_writable_roots = std::vector<std::filesystem::path>{LR"(C:\Users\owner)"}},
      fakes.files);
  auto telemetry = std::make_unique<TelemetryComponent>(
      std::move(fakes.machine.sources),
      [trust = trust.get()](std::uint32_t pid, WallTime created) {
        return trust->process(pid, created);
      },
      alerts.get());
  auto disk_health = std::make_unique<DiskHealthComponent>(
      std::move(disks), fakes.files, *alerts,
      [telemetry = telemetry.get()] { return telemetry->volumes(); });
  auto events = std::make_unique<EventsAdapter>(
      EventSources{.events = std::move(event_source),
                   .connections = std::make_unique<events::testing::FakeConnectionTable>(),
                   .processes = std::make_unique<events::testing::FakeProcessTable>()},
      trust.get());

  std::vector<std::unique_ptr<IComponent>> components;
  components.push_back(std::move(alerts));
  components.push_back(std::move(trust));
  components.push_back(std::move(telemetry));
  components.push_back(std::move(disk_health));
  components.push_back(std::move(events));
  components.push_back(std::make_unique<InputComponent>());
  return components;
}

// A console-mode service in this process with every phase-1 component over `fakes`.
inline Result<std::unique_ptr<Service>> start_phase1_service(Phase1Fakes& fakes,
                                                             const std::filesystem::path& dir,
                                                             const std::wstring& pipe,
                                                             const os::IClock& clock) {
  Result<std::string> owner = platform::current_user_sid();
  if (!owner) {
    return tl::unexpected(owner.error());
  }
  return Service::start(ServiceOptions{.database = dir / "myclean.db",
                                       .data_dir = shipped_data,
                                       .pipe_name = pipe,
                                       .owner_sid = std::move(*owner),
                                       .mode = "console",
                                       .version = std::nullopt},
                        phase1_components(fakes), clock);
}

}  // namespace myclean::svc::testing
