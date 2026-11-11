#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/model/units.hpp"
#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/format.hpp"
#include "core/tray_model/inventory.hpp"
#include "core/tray_model/screens.hpp"
#include "tray/dashboard_ui.hpp"
#include "tray/screens_ui.hpp"
#include "tray/ui_text.hpp"

// The Apps, Security and Backups screens (screens_ui.hpp).
namespace myclean::tray {

namespace {

namespace color = ui::color;
using screen_ui::action_button;
using screen_ui::ready;
using screen_ui::table_flags;
using tray_model::DashboardModel;
using tray_model::ScreenAction;
using ui::colored_text;
using ui::join;
using ui::local_date_time;
using ui::muted_text;
using ui::text;
using ui::wrapped_text;

// "Not used for 6 months", as the owner's prompt 4 asks.
constexpr std::int64_t unused_days = 180;
// The remote addresses shown per process.
constexpr std::size_t listed_remotes = 3;

std::string size_text(Bytes bytes) {
  return tray_model::format_bytes(static_cast<double>(bytes.count()));
}

unsigned verdict_color(const std::string& verdict) {
  if (verdict == "removable") {
    return color::good;
  }
  return verdict == "keep" ? color::accent : color::muted;
}

void draw_app(const tray_model::AppRow& app, bool reachable, UiState& state) {
  ImGui::TableNextRow();
  ImGui::PushID(tray_model::app_key(app).c_str());
  ImGui::TableNextColumn();
  text(app.name);
  std::vector<std::string> facts;
  facts.reserve(3);
  for (const std::string* fact : {&app.publisher, &app.version, &app.kind}) {
    if (!fact->empty()) {
      facts.push_back(*fact);
    }
  }
  muted_text(join(facts, ", "));
  wrapped_text(color::muted, app.description);
  ImGui::TableNextColumn();
  text(app.size ? size_text(*app.size) : std::string(tray_model::not_available));
  ImGui::TableNextColumn();
  wrapped_text(color::text, app.last_used_text);
  if (!app.last_used_source.empty()) {
    muted_text(app.last_used_source + ", " + app.confidence + " confidence");
  }
  ImGui::TableNextColumn();
  colored_text(verdict_color(app.verdict), app.verdict);
  wrapped_text(color::muted, app.reason);
  ImGui::TableNextColumn();
  if (app.uninstaller) {
    action_button(
        "Propose uninstall",
        ScreenAction{.kind = ScreenAction::Kind::uninstall, .keys = {tray_model::app_key(app)}},
        reachable, state);
  } else {
    muted_text("no uninstaller");
  }
  ImGui::PopID();
}

void draw_app_table(const tray_model::AppsList& list, bool reachable, UiState& state) {
  if (!ImGui::BeginTable("apps", 5, table_flags)) {
    return;
  }
  ImGui::TableSetupColumn("App", ImGuiTableColumnFlags_WidthStretch, 3.0F);
  ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.8F);
  ImGui::TableSetupColumn("Last used", ImGuiTableColumnFlags_WidthStretch, 2.0F);
  ImGui::TableSetupColumn("Verdict", ImGuiTableColumnFlags_WidthStretch, 2.0F);
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.2F);
  ImGui::TableHeadersRow();
  for (const tray_model::AppRow& app : list.apps) {
    if (!state.apps_unused_only || tray_model::unused_for(app, unused_days)) {
      draw_app(app, reachable, state);
    }
  }
  ImGui::EndTable();
}

std::string_view persistence_source_text(const std::string& source) {
  if (source == "wmi") {
    return "WMI subscription";
  }
  if (source == "ifeo") {
    return "Image File Execution Options";
  }
  if (source == "winlogon") {
    return "Winlogon";
  }
  if (source == "appinit") {
    return "AppInit_DLLs";
  }
  return tray_model::startup_source_text(source);
}

void draw_persistence(const tray_model::PersistenceRow& item, bool reachable, UiState& state) {
  ImGui::TableNextRow();
  ImGui::PushID(tray_model::persistence_key(item).c_str());
  ImGui::TableNextColumn();
  text(item.name);
  muted_text(std::string(persistence_source_text(item.source)) + ": " + item.location);
  ImGui::TableNextColumn();
  wrapped_text(color::text, item.command);
  if (!item.signer.empty()) {
    muted_text("signed by " + item.signer);
  }
  ImGui::TableNextColumn();
  unsigned shown = color::muted;
  if (tray_model::known_dangerous(item)) {
    shown = color::bad;
  } else if (item.status == "suspicious" || item.status == "known_dangerous") {
    shown = color::warning;
  }
  colored_text(shown, tray_model::trust_label(item));
  if (!item.summary.empty() && !tray_model::known_dangerous(item)) {
    wrapped_text(color::muted, item.summary);
  }
  if (!item.virustotal.empty()) {
    muted_text(item.virustotal);
  }
  ImGui::TableNextColumn();
  if (!item.image.empty()) {
    action_button("Scan with Defender",
                  ScreenAction{.kind = ScreenAction::Kind::defender_scan,
                               .keys = {tray_model::persistence_key(item)}},
                  reachable, state);
  }
  ImGui::PopID();
}

void draw_scan(const DashboardModel& model, const std::chrono::time_zone& zone, bool reachable,
               UiState& state) {
  const auto* scan = ready(model.screens().security_scan(), "Persistence points");
  if (scan == nullptr) {
    return;
  }
  text(std::format("{} Windows defaults, {} ok, {} suspicious, {} known dangerous by a verdict",
                   scan->defaults, scan->ok, scan->suspicious, scan->known_dangerous));
  if (!scan->defender_available) {
    wrapped_text(color::warning, "Microsoft Defender cannot be read: " + scan->defender_error);
  }
  for (const tray_model::DefenderDetectionRow& detection : scan->detections) {
    colored_text(color::bad,
                 std::format("Defender detected {} {}: {}", detection.threat,
                             detection.time ? local_date_time(zone, *detection.time) : "",
                             join(detection.resources, ", ")));
  }
  for (const std::string& problem : scan->problems) {
    wrapped_text(color::muted, problem);
  }
  if (!ImGui::BeginTable("persistence", 4, table_flags)) {
    return;
  }
  ImGui::TableSetupColumn("Entry", ImGuiTableColumnFlags_WidthStretch, 2.4F);
  ImGui::TableSetupColumn("Runs", ImGuiTableColumnFlags_WidthStretch, 3.0F);
  ImGui::TableSetupColumn("Assessment", ImGuiTableColumnFlags_WidthStretch, 2.4F);
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.2F);
  ImGui::TableHeadersRow();
  for (const tray_model::PersistenceRow& item : scan->items) {
    draw_persistence(item, reachable, state);
  }
  ImGui::EndTable();
}

void draw_extensions(const DashboardModel& model) {
  const auto* extensions = ready(model.screens().extensions(), "Browser extensions");
  if (extensions == nullptr) {
    return;
  }
  for (const std::string& problem : extensions->problems) {
    wrapped_text(color::muted, problem);
  }
  if (extensions->extensions.empty()) {
    muted_text("No browser extension was found.");
    return;
  }
  if (!ImGui::BeginTable("extensions", 4, table_flags)) {
    return;
  }
  for (const char* column : {"Extension", "Browser", "Installed from", "State"}) {
    ImGui::TableSetupColumn(column);
  }
  ImGui::TableHeadersRow();
  for (const tray_model::ExtensionRow& extension : extensions->extensions) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    text(extension.name + " " + extension.version);
    muted_text(extension.id);
    ImGui::TableNextColumn();
    text(extension.browser + " / " + extension.profile);
    ImGui::TableNextColumn();
    text(extension.origin);
    if (!extension.flag.empty()) {
      colored_text(color::warning, extension.flag);
    }
    ImGui::TableNextColumn();
    text(extension.enabled ? "enabled" : "disabled");
  }
  ImGui::EndTable();
}

void draw_talker(const tray_model::TalkerRow& talker) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  text(std::format("{} (pid {}){}", talker.image, talker.pid, talker.running ? "" : ", ended"));
  if (talker.unrecognized) {
    colored_text(color::warning, "unrecognized: " + join(talker.reasons, ", "));
  } else if (!talker.signer.empty()) {
    muted_text("signed by " + talker.signer);
  }
  ImGui::TableNextColumn();
  text(size_text(talker.sent) + " / " + size_text(talker.received));
  muted_text(std::format("{} minutes active", talker.active_minutes));
  ImGui::TableNextColumn();
  text(tray_model::window_text(talker.visible_window));
  ImGui::TableNextColumn();
  for (std::size_t index = 0; index < std::min(talker.remotes.size(), listed_remotes); ++index) {
    const tray_model::RemoteRow& remote = talker.remotes[index];
    muted_text(std::format("{} {}:{}{}{}", remote.protocol,
                           remote.name.empty() ? remote.address : remote.name, remote.port,
                           remote.name.empty() ? "" : " (" + remote.address + ")",
                           remote.open_now ? ", open now" : ""));
  }
  if (talker.remotes.size() > listed_remotes) {
    muted_text(std::format("and {} more", talker.remotes.size() - listed_remotes));
  }
  if (!talker.listening.empty()) {
    muted_text("listening on " + join(talker.listening, ", "));
  }
}

void draw_network(const DashboardModel& model) {
  const auto* network = ready(model.screens().network(), "Network");
  if (network == nullptr) {
    return;
  }
  if (!network->windows_reported) {
    muted_text("Which processes own a window is not reported yet.");
  }
  if (!ImGui::BeginTable("network", 4, table_flags)) {
    return;
  }
  ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthStretch, 2.6F);
  ImGui::TableSetupColumn("Sent / received", ImGuiTableColumnFlags_WidthStretch, 1.4F);
  ImGui::TableSetupColumn("Window", ImGuiTableColumnFlags_WidthStretch, 1.0F);
  ImGui::TableSetupColumn("Talks to", ImGuiTableColumnFlags_WidthStretch, 3.0F);
  ImGui::TableHeadersRow();
  for (const tray_model::TalkerRow& talker : network->processes) {
    draw_talker(talker);
  }
  ImGui::EndTable();
}

void draw_source(const tray_model::BackupSourceRow& source, const std::chrono::time_zone& zone) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  text(source.name);
  if (!source.counts_toward_age) {
    muted_text("not a backup: it lives on the volume it protects");
  }
  ImGui::TableNextColumn();
  colored_text(source.status == "found" ? color::text : color::muted,
               tray_model::backup_status_text(source.status));
  if (!source.detail.empty()) {
    wrapped_text(color::muted, source.detail);
  }
  ImGui::TableNextColumn();
  wrapped_text(color::text, source.location);
  for (const std::string& extra : source.extra) {
    wrapped_text(color::muted, extra);
  }
  ImGui::TableNextColumn();
  if (source.newest) {
    text(local_date_time(zone, *source.newest));
    muted_text(std::format("{} days ago, {} copies", source.age_days.value_or(0), source.copies));
  }
}

void draw_backup_check(const tray_model::BackupCheck& check, const std::chrono::time_zone& zone) {
  if (check.state == "checking") {
    colored_text(color::accent, "Checking...");
  }
  if (!check.error.empty()) {
    wrapped_text(color::bad, check.error);
  }
  if (check.checked_at) {
    muted_text("Checked " + local_date_time(zone, *check.checked_at));
  }
  wrapped_text(check.backup_exists ? color::text : color::warning, check.summary);
  if (check.has_last_backup && check.last_time) {
    text(std::format("Last backup: {} on {}, {} ({} days ago)", check.last_name,
                     check.last_location, local_date_time(zone, *check.last_time),
                     check.last_age_days));
  }
  if (!check.recommendation.empty()) {
    wrapped_text(color::accent, "Recommendation: " + check.recommendation);
    for (const std::string& left : check.left_out) {
      muted_text("Not suggested: " + left);
    }
  }
  if (!check.sources.empty() && ImGui::BeginTable("sources", 4, table_flags)) {
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 2.0F);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.6F);
    ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch, 2.4F);
    ImGui::TableSetupColumn("Newest", ImGuiTableColumnFlags_WidthStretch, 1.4F);
    ImGui::TableHeadersRow();
    for (const tray_model::BackupSourceRow& source : check.sources) {
      draw_source(source, zone);
    }
    ImGui::EndTable();
  }
  ImGui::SeparatorText("Repositories");
  if (!check.repositories_checked) {
    muted_text("Not checked (git is not available).");
    return;
  }
  text(std::format("{} repositories checked, {} not protected by a remote", check.repositories,
                   check.unprotected.size()));
  for (const tray_model::UnprotectedRepoRow& repository : check.unprotected) {
    wrapped_text(color::warning, repository.path + ": " + repository.text);
  }
  for (const std::string& problem : check.repository_problems) {
    wrapped_text(color::muted, problem);
  }
}

}  // namespace

void draw_apps(const DashboardModel& model, const std::chrono::time_zone& /*zone*/,
               UiState& state) {
  const bool reachable = model.service_reachable();
  wrapped_text(color::muted,
               "Every installed app with its last use and MyClean's verdict. Propose uninstall "
               "creates a plan that waits in Cleanup; once you approve it, the app's own "
               "uninstaller runs on your desktop.");
  screen_ui::request(tray_model::Screen::apps, reachable, state);
  ImGui::SameLine();
  ImGui::Checkbox("Only apps not used for 6 months", &state.apps_unused_only);
  screen_ui::draw_outcome(model);
  const auto* list = ready(model.screens().apps(), "Installed apps");
  if (list == nullptr) {
    return;
  }
  muted_text(std::format("{} apps installed; MyClean's own process history covers {} days.",
                         list->count, list->history_days));
  for (const std::string& problem : list->problems) {
    wrapped_text(color::muted, problem);
  }
  draw_app_table(*list, reachable, state);
}

void draw_security(const DashboardModel& model, const std::chrono::time_zone& zone,
                   UiState& state) {
  const bool reachable = model.service_reachable();
  wrapped_text(color::muted,
               "Nothing here removes or disables anything. Only Microsoft Defender or your opt-in "
               "VirusTotal lookup can call an item known dangerous; everything else is at most "
               "suspicious, with the reasons. A Defender scan of one item runs only when you click "
               "it, and it reports without removing anything.");
  screen_ui::request(tray_model::Screen::security, reachable, state);
  screen_ui::draw_outcome(model);
  ImGui::SeparatorText("Persistence points");
  draw_scan(model, zone, reachable, state);
  ImGui::SeparatorText("Browser extensions");
  draw_extensions(model);
  ImGui::SeparatorText("Network, last 24 hours");
  draw_network(model);
}

void draw_backups(const DashboardModel& model, const std::chrono::time_zone& zone, UiState& state) {
  const bool reachable = model.service_reachable();
  wrapped_text(color::muted,
               "Every backup source of this PC and the age of the newest backup. MyClean only "
               "reads: it sets nothing up.");
  screen_ui::request(tray_model::Screen::backups, reachable, state);
  if (const auto* check = ready(model.screens().backups(), "Backups")) {
    draw_backup_check(*check, zone);
  }
}

}  // namespace myclean::tray
