#pragma once

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include "core/clean/probes.hpp"
#include "core/cleaning/executor.hpp"
#include "core/cleaning/service.hpp"
#include "core/cleanup/command_runner.hpp"
#include "core/cleanup/handler_host.hpp"
#include "core/cleanup/recycle_bin.hpp"
#include "core/devdrive/advisor.hpp"
#include "core/health/analysis.hpp"
#include "core/indexer/scope.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/os/file_system.hpp"
#include "core/packs/applies.hpp"
#include "core/packs/loader.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/settings.hpp"
#include "core/space/downloads.hpp"
#include "core/space/duplicates.hpp"
#include "core/space/installers.hpp"
#include "core/space/photos.hpp"
#include "core/space/scanner.hpp"
#include "core/startup/diagnostics.hpp"
#include "core/startup/startup.hpp"
#include "core/store/migrations.hpp"
#include "core/vhdx/compact.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

// The machine the cleaning component works on: Win32 adapters in the service, fakes in tests.
// Every one may be null except file_system, tree and usage (see cleaning::Probes).
struct CleaningSources {
  std::unique_ptr<os::IFileSystem> file_system;
  std::unique_ptr<clean::IFileTree> tree;
  std::unique_ptr<clean::IUsage> usage;
  std::unique_ptr<clean::IGit> git;
  std::unique_ptr<cleanup::IHandlerHost> handlers;  // system-area handlers
  std::unique_ptr<cleanup::ICommandRunner> commands;
  std::filesystem::path dism;
  std::unique_ptr<placement::ISystemSpace> system;
  std::unique_ptr<placement::IProcessRunner> tools;
  std::unique_ptr<placement::IDriveFacts> drives;
  std::unique_ptr<space::IDirectoryReader> reader;
  std::unique_ptr<space::IZoneIdentifierReader> zones;
  std::unique_ptr<space::IContentHasher> hasher;
  std::unique_ptr<space::IInstallerInspector> installers;
  std::unique_ptr<space::IImageDecoder> images;
  std::unique_ptr<devdrive::IDevDriveProbe> dev_drive;
  std::unique_ptr<vhdx::IVirtualDisks> disks;  // compacts virtual disks
  // The owner's session context for the owner with this SID (see svc::owner_context).
  std::function<Result<cleaning::OwnerContext>(std::string_view owner_sid)> owner;
  std::function<bool()> busy;  // the system volume is critically busy
  // What the knowledge packs ask about the machine, for the owner with this SID; empty: every
  // loaded pack applies.
  std::function<std::unique_ptr<packs::IMachine>(std::string_view owner_sid)> machine;
  // The owner's override packs folder (platform/packs); empty: no owner packs.
  std::function<Result<std::optional<std::filesystem::path>>()> owner_packs;
  // The $I records of the bins of the owner with this SID; empty: no aged Recycle Bin items.
  std::function<std::unique_ptr<cleanup::IBinRecords>(std::string_view owner_sid)> bin;
  // The startup items of the owner with this SID and of the machine; empty: no startup items.
  std::function<std::unique_ptr<startup::IStartupSystem>(std::string_view owner_sid)> startup;
  // Windows' Diagnostics-Performance boot events; null: none.
  std::unique_ptr<startup::IDiagnosticsLog> diagnostics;
  // The Windows Search crawl scope; null: no indexer-scope check.
  std::unique_ptr<indexer::IIndexerScope> indexer;
  // The alert sink plan verifications report to (must outlive the component); null: none.
  health::IAlertSink* alerts = nullptr;
};

// How this service runs cleaning.
struct CleaningMode {
  // A console run executes nothing unless it has a sandbox, and never applies Windows
  // handlers, DISM or system settings.
  bool console = false;
  // Console only: packs and workspaces come from <sandbox>\data (no owner packs), quarantined items
  // go to <sandbox>\quarantine, and the plans run (on the files the sandbox rules select).
  std::optional<std::filesystem::path> sandbox;
  std::filesystem::path system_root{L"C:\\"};
};

// Cleaning as a service component (cleaning::CleaningService): the cleaning worker (owner
// session, scheduled auto run and purge, scans, approved plans) and the space worker on threads
// of their own, woken by requests and every few seconds. Registers the methods of
// core/ipc/plans.hpp.
class CleaningComponent final : public IComponent {
 public:
  static constexpr std::chrono::seconds poll_interval{5};

  // `files` must outlive the component.
  CleaningComponent(CleaningSources sources, const packs::IPackFiles& files, CleaningMode mode);
  CleaningComponent(const CleaningComponent&) = delete;
  CleaningComponent& operator=(const CleaningComponent&) = delete;
  CleaningComponent(CleaningComponent&&) = delete;
  CleaningComponent& operator=(CleaningComponent&&) = delete;
  ~CleaningComponent() override;

  [[nodiscard]] std::string_view name() const override { return "cleaning"; }
  [[nodiscard]] std::vector<store::MigrationSet> migrations() const override;
  [[nodiscard]] Result<void> start(const Context& context) override;
  void stop() override;
  [[nodiscard]] ipc::ComponentHealth health() const override;
  [[nodiscard]] std::vector<ipc::PackStatus> pack_status() const override;

 private:
  void loop(const std::stop_token& stop, bool space);
  void wake();

  CleaningSources sources_;
  const packs::IPackFiles* files_;
  std::unique_ptr<packs::IMachine> machine_;
  std::unique_ptr<cleanup::IBinRecords> bin_;
  std::unique_ptr<startup::IStartupSystem> startup_;
  CleaningMode mode_;
  std::unique_ptr<cleaning::CleaningService> service_;
  std::mutex mutex_;
  std::condition_variable_any woken_;
  int wakes_ = 0;  // guarded by mutex_
  std::jthread worker_;
  std::jthread space_worker_;
};

}  // namespace myclean::svc
