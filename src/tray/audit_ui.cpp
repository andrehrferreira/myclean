#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/tray_model/audit.hpp"
#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/format.hpp"
#include "core/tray_model/screens.hpp"
#include "tray/dashboard_ui.hpp"
#include "tray/screens_ui.hpp"
#include "tray/ui_text.hpp"

// The Audit and Settings screens (screens_ui.hpp).
namespace myclean::tray {

namespace {

namespace color = ui::color;
using screen_ui::ready;
using screen_ui::table_flags;
using tray_model::DashboardModel;
using ui::colored_text;
using ui::join;
using ui::local_date_time;
using ui::muted_text;
using ui::text;
using ui::wrapped_text;

void draw_entry(const tray_model::AuditEntryRow& entry, const std::chrono::time_zone& zone) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  muted_text(std::to_string(entry.id));
  ImGui::TableNextColumn();
  muted_text(local_date_time(zone, entry.time));
  ImGui::TableNextColumn();
  text(entry.plan ? std::format("plan {}", *entry.plan) : std::string("-"));
  ImGui::TableNextColumn();
  text(entry.actor);
  ImGui::TableNextColumn();
  text(entry.event);
  ImGui::TableNextColumn();
  wrapped_text(color::text, tray_model::audit_text(entry));
}

// Newer and Older: the page below the newest shown, or back to the one above.
void draw_paging(const tray_model::AuditPage& page, bool reachable, UiState& state) {
  ImGui::BeginDisabled(!reachable || state.audit_pages.empty());
  if (ImGui::Button("Newer")) {
    state.audit_pages.pop_back();
    state.requests.emplace_back(tray_model::LoadScreen{
        .screen = tray_model::Screen::audit,
        .before =
            state.audit_pages.empty() ? std::nullopt : std::optional(state.audit_pages.back())});
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!reachable || !page.next);
  if (ImGui::Button("Older") && page.next) {
    state.audit_pages.push_back(*page.next);
    state.requests.emplace_back(
        tray_model::LoadScreen{.screen = tray_model::Screen::audit, .before = *page.next});
  }
  ImGui::EndDisabled();
}

void draw_governor_settings(const DashboardModel& model, const std::chrono::time_zone& zone) {
  const auto& governor = model.governor();
  if (!governor) {
    muted_text("Governor: " + std::string(tray_model::not_available));
    return;
  }
  text(governor->mode == "observe"
           ? "Mode: observe (records what it would do, changes nothing; mode in governor.toml)"
           : "Mode: act (mode in governor.toml)");
  if (governor->paused_until) {
    colored_text(color::warning, "Paused until " + local_date_time(zone, *governor->paused_until) +
                                     " (end the pause in Governor)");
  } else {
    muted_text("Not paused (pause it in Governor).");
  }
  text("Game mode: " + governor->game_override);
}

void draw_snoozes(const DashboardModel& model, const std::chrono::time_zone& zone) {
  const auto& alerts = model.alerts();
  const std::optional<WallTime> now = model.metrics_time();
  if (!alerts || !now) {
    muted_text("Snoozed alerts: " + std::string(tray_model::not_available));
    return;
  }
  const std::vector<const tray_model::AlertRow*> snoozed =
      tray_model::snoozed_alerts(*alerts, *now);
  if (snoozed.empty()) {
    muted_text("No alert is snoozed.");
    return;
  }
  for (const tray_model::AlertRow* alert : snoozed) {
    text(std::format("{} ({}): snoozed until {}", alert->title, alert->entity,
                     local_date_time(zone, alert->snoozed_until.value_or(*now))));
  }
}

void draw_virustotal(const DashboardModel& model) {
  const auto* scan = ready(model.screens().security_scan(), "VirusTotal");
  if (scan == nullptr) {
    return;
  }
  text(scan->virustotal_enabled ? "VirusTotal lookups: on (only the SHA-256 of suspicious files)"
                                : "VirusTotal lookups: off");
  if (!scan->virustotal_note.empty()) {
    muted_text(scan->virustotal_note);
  }
  if (scan->virustotal_queued > 0) {
    muted_text(std::format("{} lookups wait.", scan->virustotal_queued));
  }
  wrapped_text(color::muted,
               "Turned on with its API key in %ProgramData%\\MyClean\\security.toml, which only "
               "administrators can change; the tray never shows or stores the key.");
}

void draw_workspaces(const DashboardModel& model) {
  const auto* check = ready(model.screens().backups(), "Workspace roots");
  if (check == nullptr) {
    return;
  }
  if (check->workspace_roots.empty()) {
    muted_text(check->repositories_checked
                   ? "No workspace root is set (data/workspaces.toml)."
                   : "Not reported yet: the backup check has not looked at the repositories.");
    return;
  }
  text("Workspace roots (data/workspaces.toml): " + join(check->workspace_roots, ", "));
}

}  // namespace

void draw_audit(const DashboardModel& model, const std::chrono::time_zone& zone, UiState& state) {
  const bool reachable = model.service_reachable();
  wrapped_text(color::muted,
               "Every change MyClean made or was asked to make, as recorded in its append-only "
               "log, newest first. Nothing here can change or remove an entry.");
  screen_ui::request(tray_model::Screen::audit, reachable, state);
  const auto* page = ready(model.screens().audit(), "Audit log");
  if (page == nullptr) {
    return;
  }
  ImGui::SameLine();
  draw_paging(*page, reachable, state);
  if (page->entries.empty()) {
    muted_text("The log is empty.");
    return;
  }
  if (!ImGui::BeginTable("audit", 6, table_flags)) {
    return;
  }
  ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthStretch, 0.4F);
  ImGui::TableSetupColumn("When", ImGuiTableColumnFlags_WidthStretch, 1.2F);
  ImGui::TableSetupColumn("Plan", ImGuiTableColumnFlags_WidthStretch, 0.6F);
  ImGui::TableSetupColumn("Who", ImGuiTableColumnFlags_WidthStretch, 0.9F);
  ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch, 0.9F);
  ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthStretch, 4.0F);
  ImGui::TableHeadersRow();
  for (const tray_model::AuditEntryRow& entry : page->entries) {
    draw_entry(entry, zone);
  }
  ImGui::EndTable();
}

void draw_settings(const DashboardModel& model, const std::chrono::time_zone& zone,
                   UiState& state) {
  const bool reachable = model.service_reachable();
  wrapped_text(color::muted,
               "The settings the service reports, shown as they are. They are changed in their "
               "own screens (pause in Governor, snooze in Alerts) or in MyClean's data files.");
  screen_ui::request(tray_model::Screen::settings, reachable, state);
  ImGui::SeparatorText("Governor");
  draw_governor_settings(model, zone);
  ImGui::SeparatorText("Snoozed alerts");
  draw_snoozes(model, zone);
  ImGui::SeparatorText("VirusTotal");
  draw_virustotal(model);
  ImGui::SeparatorText("Workspaces");
  draw_workspaces(model);
}

}  // namespace myclean::tray
