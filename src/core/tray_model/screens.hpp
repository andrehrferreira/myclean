#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/tray_model/audit.hpp"
#include "core/tray_model/inventory.hpp"
#include "core/tray_model/optimizer.hpp"
#include "core/tray_model/reports.hpp"
#include "core/tray_model/service_link.hpp"

// The dashboard's screens over the optimizer, inventory, audit and report methods (PRD UI-3):
// what each screen asks the service for, the owner's proposals from them, and their answers as
// the dashboard shows them. A screen loads when the owner opens it and again on Refresh; nothing
// it offers changes the machine by itself: every change is a plan that waits for the owner's
// approval in Cleanup.
namespace myclean::tray_model {

enum class Screen {
  placement,
  optimize,
  startup,
  apps,
  security,
  backups,
  audit,
  settings,
  reports
};

// Loads the answers of `screen`; `refresh` starts a new analysis where the method offers one.
struct LoadScreen {
  Screen screen = Screen::placement;
  bool refresh = false;
  std::optional<std::int64_t> before;  // audit: the page below this entry id; nullopt: the newest
  // reports: the list when empty, else the report of `month`, or with `other` the changes from
  // snapshot `month` to snapshot `other`.
  std::string month;
  std::string other;
};

// One method call of a screen.
struct ScreenCall {
  std::string method;
  nlohmann::json params;

  friend bool operator==(const ScreenCall&, const ScreenCall&) = default;
};

// The calls that load `load`'s screen.
[[nodiscard]] std::vector<ScreenCall> screen_calls(const LoadScreen& load);

// The owner's proposal from a screen. Items are named only by the integer ids the service
// answered with. An item whose id is its number in the latest list answer is held by its key
// instead (a startup item, an app, a persistence point): the proposal lists the
// items again and sends their fresh ids, so that a list another client asked for meanwhile cannot
// shift them.
struct ScreenAction {
  enum class Kind {
    system_setting,
    relocation,
    startup,
    index_exclusion,
    compact,
    uninstall,     // one app: a plan that runs its own uninstaller once approved
    defender_scan  // one persistence point: a Defender scan that remediates nothing
  };
  Kind kind = Kind::system_setting;
  std::string value;              // system_setting: its kind; relocation: the cache
  std::vector<std::int64_t> ids;  // index_exclusion, compact: candidate ids
  std::vector<std::string> keys;  // startup, uninstall, defender_scan: the items' keys
  bool enable = false;            // startup: enable them instead of disabling them
};

// The list call whose fresh ids a keyed action needs; nullopt for an action named by ids.
[[nodiscard]] std::optional<ScreenCall> list_call(const ScreenAction& action);
// The ids of `action`'s keys in `list` (the answer of list_call). Fails when one is not listed
// any more.
[[nodiscard]] Result<std::vector<std::int64_t>> fresh_ids(const ScreenAction& action,
                                                          const nlohmann::json& list);
// The proposal call of `action` over `ids` (its own ids, or the fresh ones of its keys).
[[nodiscard]] ScreenCall action_call(const ScreenAction& action,
                                     const std::vector<std::int64_t>& ids);

// One answer, or why the call failed.
struct ScreenAnswer {
  std::string method;
  Result<nlohmann::json> reply;
};

// How the owner's last proposal went.
struct ActionOutcome {
  std::string text;  // "Plan 12 waits for your approval in Cleanup.", or why it failed
  bool failed = false;
};

// What one poll learned for the screens.
struct ScreensPoll {
  std::vector<ScreenAnswer> answers;
  std::optional<ActionOutcome> action;
};

// Sends the owner's proposal: lists the items again when it holds keys, then proposes; the fresh
// list answer joins `poll`.
void send_action(IServiceLink& link, const ScreenAction& action, ScreensPoll& poll);

// Loads the screens for the poller and asks again, at every poll, for each answer whose
// analysis was still running ("scanning", "checking"). Not thread-safe.
class ScreenFetcher {
 public:
  void load(IServiceLink& link, const LoadScreen& load, ScreensPoll& poll);
  // Leaves out the calls of `loading`, the screens this poll loads anyway.
  void ask_again(IServiceLink& link, const std::vector<LoadScreen>& loading, ScreensPoll& poll);
  // The dashboard closed: nothing is asked again.
  void clear() { running_.clear(); }

 private:
  void call(IServiceLink& link, const ScreenCall& call, ScreensPoll& poll);

  std::vector<ScreenCall> running_;
};

// The newest answer of a screen method: nullopt before it was asked for, else the parsed answer
// or why the call or its parsing failed.
template <typename T>
using Answer = std::optional<Result<T>>;

// The screens' answers, built from poll results. Not thread-safe.
class ScreensView {
 public:
  void apply(ScreensPoll poll);

  [[nodiscard]] const Answer<std::vector<SystemSettingRow>>& system_space() const {
    return system_space_;
  }
  [[nodiscard]] const Answer<IndexedFolders>& indexed_folders() const { return indexed_; }
  [[nodiscard]] const Answer<VirtualDisks>& virtual_disks() const { return vhdx_; }
  [[nodiscard]] const Answer<DevDriveAdvice>& dev_drive() const { return dev_drive_; }
  [[nodiscard]] const Answer<std::vector<ResidentCostRow>>& resident_costs() const {
    return costs_;
  }
  [[nodiscard]] const Answer<std::vector<StartupItemRow>>& startup_items() const {
    return startup_;
  }
  [[nodiscard]] const Answer<BootTimes>& boot_times() const { return boot_times_; }
  [[nodiscard]] const Answer<AppsList>& apps() const { return apps_; }
  [[nodiscard]] const Answer<SecurityScan>& security_scan() const { return scan_; }
  [[nodiscard]] const Answer<Extensions>& extensions() const { return extensions_; }
  [[nodiscard]] const Answer<NetworkTalkers>& network() const { return network_; }
  [[nodiscard]] const Answer<BackupCheck>& backups() const { return backups_; }
  [[nodiscard]] const Answer<AuditPage>& audit() const { return audit_; }
  [[nodiscard]] const Answer<std::vector<ReportRow>>& reports() const { return reports_; }
  // The report or comparison the owner opened last.
  [[nodiscard]] const Answer<ReportText>& report() const { return report_; }
  // How the owner's last proposal went; nullopt before the first.
  [[nodiscard]] const std::optional<ActionOutcome>& action() const { return action_; }

 private:
  Answer<std::vector<SystemSettingRow>> system_space_;
  Answer<IndexedFolders> indexed_;
  Answer<VirtualDisks> vhdx_;
  Answer<DevDriveAdvice> dev_drive_;
  Answer<std::vector<ResidentCostRow>> costs_;
  Answer<std::vector<StartupItemRow>> startup_;
  Answer<BootTimes> boot_times_;
  Answer<AppsList> apps_;
  Answer<SecurityScan> scan_;
  Answer<Extensions> extensions_;
  Answer<NetworkTalkers> network_;
  Answer<BackupCheck> backups_;
  Answer<AuditPage> audit_;
  Answer<std::vector<ReportRow>> reports_;
  Answer<ReportText> report_;
  std::optional<ActionOutcome> action_;
};

// The error text of a failed answer, as a screen shows it: "n/a (<why>)".
[[nodiscard]] std::string answer_error(const Error& error);

}  // namespace myclean::tray_model
