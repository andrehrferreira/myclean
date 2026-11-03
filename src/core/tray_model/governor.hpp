#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

// The tray's side of the resource governor (contracts in core/ipc/governor.hpp; governor spec,
// "Rate-Limited Owner Notifications", "Pause Governor", "Game Mode"): governor.status as the
// dashboard shows it, the parameters of the owner's choices, and the toasts of new notifications.
namespace myclean::tray_model {

// A process instance as the service names it: PID and creation time in nanoseconds since the
// Unix epoch.
struct GovernorProcess {
  std::uint32_t pid = 0;
  std::int64_t created = 0;
  friend auto operator<=>(const GovernorProcess&, const GovernorProcess&) = default;
};

// One pending owner notification: a tree the governor acted on.
struct GovernorNotification {
  std::int64_t id = 0;
  WallTime time;
  GovernorProcess root;
  std::string image;
  std::string process_class;
  std::string step;  // priority, ecoqos, cap_job, suspend, kill
  std::string reason;
  std::int64_t members = 0;
};

// A tree with a ladder step in effect.
struct GovernorTree {
  GovernorProcess root;
  std::string image;
  std::string process_class;
  std::string step;
};

struct GovernorGame {
  std::string image;
  std::string reason;  // owner_list, library_root, game_config_store, fullscreen_gpu
};

// One governor_event row, as the dashboard lists it.
struct GovernorEventRow {
  WallTime time;
  std::string kind;
  std::string image;
  std::optional<std::string> step;
  std::string reason;
  std::string actor;
  std::string detail;
};

// "End <trees> loops of <pattern>": orphaned polling loops the owner is asked about.
struct OrphanProposalRow {
  std::int64_t id = 0;
  WallTime time;
  std::string pattern;
  std::string image;
  std::int64_t trees = 0;
  std::int64_t processes = 0;
};

// The owner's stored decision for a pattern.
struct OrphanRuleRow {
  std::int64_t id = 0;
  std::string pattern;
  std::string image;
  std::string rule;  // always, ignore
  WallTime time;
};

// governor.status.
struct GovernorStatus {
  std::string mode;  // observe, act
  std::optional<WallTime> paused_until;
  bool pressure = false;
  bool clearing = false;
  std::vector<std::string> reasons;
  bool game_mode = false;
  std::string game_override;  // auto, on, off
  int build_cpu_percent = 0;
  std::vector<GovernorGame> games;
  std::vector<GovernorTree> trees;
  std::vector<GovernorNotification> notifications;  // oldest first
  std::vector<OrphanProposalRow> orphan_proposals;
  std::vector<OrphanRuleRow> orphan_rules;
  std::vector<GovernorEventRow> events;  // newest first
};

[[nodiscard]] Result<GovernorStatus> parse_governor_status(const nlohmann::json& reply);

// The owner's answers (core/ipc/governor.hpp). `choice`: resume, kill, allow_once, always_allow;
// `value`: auto, on, off.
[[nodiscard]] nlohmann::json governor_choose_params(const GovernorProcess& root,
                                                    std::string_view choice);
[[nodiscard]] nlohmann::json governor_pause_params(std::chrono::minutes minutes);
[[nodiscard]] nlohmann::json governor_game_mode_params(std::string_view value);
// `decision`: end_now, always, ignore (`id` a proposal) or forget (`id` a rule).
[[nodiscard]] nlohmann::json governor_orphans_params(std::int64_t id, std::string_view decision);

// What the tray reads in the owner's session every second for governor.user_state: the
// SHQueryUserNotificationState token and the foreground window's process (0: none).
struct UserStateReading {
  std::string state;
  std::uint32_t foreground_pid = 0;
};

[[nodiscard]] nlohmann::json governor_user_state_params(const UserStateReading& reading);

// A toast for the owner.
struct GovernorToast {
  std::string title;
  std::string detail;
  bool severe = false;  // a suspend or a kill
};

// The text of the toast of `notification`: what the governor did to which program, and where the
// owner answers.
[[nodiscard]] GovernorToast governor_toast(const GovernorNotification& notification);
// The toast of an orphaned-loop proposal: "End 88 loops of <pattern>?".
[[nodiscard]] GovernorToast orphan_toast(const OrphanProposalRow& proposal);

// Decides which notifications become toasts: each one newer than every notification seen before
// (by its time, then its id, so a restarted service whose ids start again is still noticed), at
// most one per tree per `interval` by the notifications' own times; a suspend or kill is always
// shown, since the tree is stopped until the owner acts. The first status only sets the baseline:
// notifications pending before the tray started are listed in the dashboard and badge the icon,
// they are not toasted. Each orphaned-loop proposal newer than every one seen before is toasted
// once, in act mode only (in observe mode ending is only recorded, so it waits in the dashboard).
class GovernorToasts {
 public:
  static constexpr std::chrono::minutes default_interval{5};

  explicit GovernorToasts(std::chrono::minutes interval = default_interval) : interval_(interval) {}

  [[nodiscard]] std::vector<GovernorToast> take(const GovernorStatus& status);

 private:
  std::chrono::minutes interval_;
  bool baseline_ = false;
  std::optional<std::pair<WallTime, std::int64_t>> newest_;
  std::optional<std::pair<WallTime, std::int64_t>> newest_proposal_;
  // Vectors, so that moving the dashboard model never throws.
  std::vector<std::pair<GovernorProcess, WallTime>> last_by_tree_;
};

}  // namespace myclean::tray_model
