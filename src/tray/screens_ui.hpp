#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include <imgui.h>

#include "core/model/result.hpp"
#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/screens.hpp"
#include "tray/dashboard_ui.hpp"
#include "tray/ui_text.hpp"

// The dashboard's screens over the optimizer, inventory, audit and report methods (PRD UI-3),
// drawn into the current ImGui frame; the owner's clicks become requests in `state`. Every change
// they offer is a proposal that waits for the owner's approval in Cleanup.
namespace myclean::tray {

// Placement: hibernation, the page file and the restore-point storage of the system drive with
// their cost and a Propose button for the recommended change, and the relocatable caches with
// their newest relocation plan and a Propose button.
void draw_placement(const tray_model::DashboardModel& model, const std::chrono::time_zone& zone,
                    UiState& state);
// Optimize: the applied optimizations with their verification, the build and cache folders the
// search index holds (propose an exclusion), the WSL and Docker virtual disks (propose a
// compaction), the Dev Drive advice and the 30-day cost of the resident programs.
void draw_optimize(const tray_model::DashboardModel& model, const std::chrono::time_zone& zone,
                   UiState& state);
// Startup: every startup item with its impact, ticked to propose disabling or enabling them, and
// the boot and logon times with the effect of earlier changes.
void draw_startup(const tray_model::DashboardModel& model, const std::chrono::time_zone& zone,
                  UiState& state);

// Apps: every installed app with its last use, its source and MyClean's verdict, optionally only
// those not used for 6 months, each with a Propose uninstall button.
void draw_apps(const tray_model::DashboardModel& model, const std::chrono::time_zone& zone,
               UiState& state);
// Security: the persistence points with their trust label (known dangerous only by a verdict) and
// a Defender scan button per item, the browser extensions with their origin and the network
// talkers of the last 24 hours.
void draw_security(const tray_model::DashboardModel& model, const std::chrono::time_zone& zone,
                   UiState& state);
// Backups: every backup source, the last-backup age, the recommendation and the repositories
// without a remote.
void draw_backups(const tray_model::DashboardModel& model, const std::chrono::time_zone& zone,
                  UiState& state);

// Audit: the action log newest first, a page at a time, read only.
void draw_audit(const tray_model::DashboardModel& model, const std::chrono::time_zone& zone,
                UiState& state);
// Settings: the governor's mode and pause, the snoozed alerts, whether VirusTotal lookups are on
// and the workspace roots, shown and never edited here.
void draw_settings(const tray_model::DashboardModel& model, const std::chrono::time_zone& zone,
                   UiState& state);

// Reports: the monthly reports with Open, which shows the report's text here, and a comparison of
// any two snapshots.
void draw_reports(const tray_model::DashboardModel& model, const std::chrono::time_zone& zone,
                  UiState& state);

namespace screen_ui {

inline constexpr ImGuiTableFlags table_flags =
    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp |
    ImGuiTableFlags_PadOuterX;

// The Refresh button; loads `screen` once when it is first shown, and again on Refresh.
void request(tray_model::Screen screen, bool reachable, UiState& state);
// How the owner's last proposal went.
void draw_outcome(const tray_model::DashboardModel& model);
// Sends `action` when the button `label` is clicked.
void action_button(const char* label, tray_model::ScreenAction action, bool reachable,
                   UiState& state);
// The parsed answer; null, with "Loading..." before it arrived or the error of a failed one.
template <typename T>
const T* ready(const tray_model::Answer<T>& answer, std::string_view what) {
  if (!answer) {
    ui::muted_text("Loading...");
    return nullptr;
  }
  if (!answer->has_value()) {
    ui::wrapped_text(ui::color::bad,
                     std::string(what) + ": " + tray_model::answer_error(answer->error()));
    return nullptr;
  }
  return &**answer;
}
// An analysis' state line: "Analysing...", its error, "As of <time>".
void draw_analysis(const tray_model::AnalysisState& analysis, const std::chrono::time_zone& zone);

}  // namespace screen_ui

}  // namespace myclean::tray
