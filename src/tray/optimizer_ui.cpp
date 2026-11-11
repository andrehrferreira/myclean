#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "core/ipc/json_rpc.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/tray_model/cleaning.hpp"
#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/format.hpp"
#include "core/tray_model/optimizer.hpp"
#include "core/tray_model/screens.hpp"
#include "tray/dashboard_ui.hpp"
#include "tray/screens_ui.hpp"
#include "tray/ui_text.hpp"

// The Placement, Optimize and Startup screens (screens_ui.hpp).
namespace myclean::tray {

namespace {

namespace color = ui::color;
using screen_ui::action_button;
using screen_ui::ready;
using screen_ui::table_flags;
using tray_model::DashboardModel;
using tray_model::ScreenAction;
using ui::colored_text;
using ui::local_date_time;
using ui::muted_text;
using ui::text;
using ui::wrapped_text;

// The resident programs listed, most CPU time first.
constexpr std::size_t listed_costs = 15;

std::string size_text(Bytes bytes) {
  return tray_model::format_bytes(static_cast<double>(bytes.count()));
}

std::string seconds_text(double seconds) {
  return std::format("{:.1f} s", seconds);
}

// "+2.5 s" / "-1.0 s" from the median.
std::string difference_text(const std::optional<double>& difference) {
  return difference ? std::format(" ({:+.1f} s from the median)", *difference) : std::string();
}

// A plan as one line: "plan 12, verified".
std::string plan_text(const tray_model::PlanRow& plan) {
  return std::format("plan {}, {}", plan.id, tray_model::state_text(plan));
}

void draw_setting(const tray_model::SystemSettingRow& setting, bool reachable, UiState& state) {
  ImGui::TableNextRow();
  ImGui::PushID(setting.kind.c_str());
  ImGui::TableNextColumn();
  text(tray_model::setting_name(setting.kind));
  ImGui::TableNextColumn();
  if (!setting.error.empty()) {
    wrapped_text(color::muted, setting.error);
  } else {
    wrapped_text(color::text, setting.current);
  }
  ImGui::TableNextColumn();
  text(setting.cost ? size_text(*setting.cost) : std::string(tray_model::not_available));
  ImGui::TableNextColumn();
  if (setting.recommended) {
    wrapped_text(color::accent,
                 *setting.recommended + (setting.restart_required ? " (after a restart)" : ""));
    action_button("Propose",
                  ScreenAction{.kind = ScreenAction::Kind::system_setting, .value = setting.kind},
                  reachable, state);
  } else if (setting.error.empty()) {
    muted_text("no change needed");
  }
  ImGui::PopID();
}

void draw_system_space(const DashboardModel& model, bool reachable, UiState& state) {
  const auto* settings = ready(model.screens().system_space(), "System settings");
  if (settings == nullptr || !ImGui::BeginTable("settings", 4, table_flags)) {
    return;
  }
  ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 1.6F);
  ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthStretch, 2.4F);
  ImGui::TableSetupColumn("Holds", ImGuiTableColumnFlags_WidthStretch, 0.8F);
  ImGui::TableSetupColumn("Recommended", ImGuiTableColumnFlags_WidthStretch, 2.4F);
  ImGui::TableHeadersRow();
  for (const tray_model::SystemSettingRow& setting : *settings) {
    draw_setting(setting, reachable, state);
  }
  ImGui::EndTable();
}

void draw_relocations(const DashboardModel& model, bool reachable, UiState& state) {
  const auto& plans = model.cleanup().plans();
  if (!plans) {
    muted_text("Relocations: " + std::string(tray_model::not_available));
    return;
  }
  if (!ImGui::BeginTable("relocations", 3, table_flags)) {
    return;
  }
  ImGui::TableSetupColumn("Cache", ImGuiTableColumnFlags_WidthStretch, 1.0F);
  ImGui::TableSetupColumn("Newest plan", ImGuiTableColumnFlags_WidthStretch, 4.0F);
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.4F);
  ImGui::TableHeadersRow();
  for (const tray_model::RelocationRow& row : tray_model::relocations(*plans)) {
    ImGui::TableNextRow();
    ImGui::PushID(row.plan != nullptr ? static_cast<int>(row.plan->id) : -1);
    ImGui::PushID(row.cache.c_str());
    ImGui::TableNextColumn();
    text(row.cache.empty() ? "knowledge pack" : row.cache);
    ImGui::TableNextColumn();
    if (row.plan == nullptr) {
      muted_text("not relocated");
    } else {
      wrapped_text(color::text, row.plan->title);
      muted_text(plan_text(*row.plan));
    }
    ImGui::TableNextColumn();
    if (!row.cache.empty()) {
      action_button("Propose relocation",
                    ScreenAction{.kind = ScreenAction::Kind::relocation, .value = row.cache},
                    reachable, state);
    }
    ImGui::PopID();
    ImGui::PopID();
  }
  ImGui::EndTable();
}

void draw_applied(const DashboardModel& model) {
  const auto& plans = model.cleanup().plans();
  if (!plans) {
    muted_text("Applied optimizations: " + std::string(tray_model::not_available));
    return;
  }
  const std::vector<const tray_model::PlanRow*> applied = tray_model::applied_optimizations(*plans);
  if (applied.empty()) {
    muted_text("None applied yet.");
    return;
  }
  if (!ImGui::BeginTable("applied", 3, table_flags)) {
    return;
  }
  ImGui::TableSetupColumn("Change", ImGuiTableColumnFlags_WidthStretch, 3.0F);
  ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 1.6F);
  ImGui::TableSetupColumn("Measured effect", ImGuiTableColumnFlags_WidthStretch, 1.4F);
  ImGui::TableHeadersRow();
  for (const tray_model::PlanRow* plan : applied) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    wrapped_text(color::text, std::format("{} (plan {})", plan->title, plan->id));
    ImGui::TableNextColumn();
    text(tray_model::state_text(*plan));
    ImGui::TableNextColumn();
    const std::string_view effect = tray_model::verification_text(plan->verification);
    unsigned shown = color::muted;
    if (plan->verification == "better") {
      shown = color::good;
    } else if (plan->verification == "worse") {
      shown = color::bad;
    }
    colored_text(shown, effect);
  }
  ImGui::EndTable();
}

void draw_indexed(const DashboardModel& model, const std::chrono::time_zone& zone, bool reachable,
                  UiState& state) {
  const auto* indexed = ready(model.screens().indexed_folders(), "Indexed folders");
  if (indexed == nullptr) {
    return;
  }
  screen_ui::draw_analysis(indexed->analysis, zone);
  if (indexed->folders.empty()) {
    muted_text("No build or cache folder is in the search index.");
    return;
  }
  // A proposal names at most ipc::max_record_elements candidates.
  if (indexed->folders.size() <= ipc::max_record_elements) {
    std::vector<std::int64_t> all;
    all.reserve(indexed->folders.size());
    for (const tray_model::IndexedFolderRow& folder : indexed->folders) {
      all.push_back(folder.id);
    }
    action_button(std::format("Propose excluding all {}", all.size()).c_str(),
                  ScreenAction{.kind = ScreenAction::Kind::index_exclusion, .ids = all}, reachable,
                  state);
  }
  if (!ImGui::BeginTable("indexed", 3, table_flags)) {
    return;
  }
  ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch, 4.0F);
  ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthStretch, 1.6F);
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.0F);
  ImGui::TableHeadersRow();
  for (const tray_model::IndexedFolderRow& folder : indexed->folders) {
    ImGui::TableNextRow();
    ImGui::PushID(static_cast<int>(folder.id));
    ImGui::TableNextColumn();
    wrapped_text(color::text, folder.path);
    ImGui::TableNextColumn();
    muted_text(folder.label.empty() ? folder.category : folder.label);
    ImGui::TableNextColumn();
    action_button("Exclude",
                  ScreenAction{.kind = ScreenAction::Kind::index_exclusion, .ids = {folder.id}},
                  reachable, state);
    ImGui::PopID();
  }
  ImGui::EndTable();
}

void draw_disks(const DashboardModel& model, const std::chrono::time_zone& zone, bool reachable,
                UiState& state) {
  const auto* disks = ready(model.screens().virtual_disks(), "Virtual disks");
  if (disks == nullptr) {
    return;
  }
  screen_ui::draw_analysis(disks->analysis, zone);
  for (const std::string& note : disks->notes) {
    wrapped_text(color::muted, note);
  }
  if (disks->disks.empty()) {
    muted_text("No WSL or Docker virtual disk was found.");
    return;
  }
  wrapped_text(color::muted,
               std::format("{} in all. Compacting first stops every WSL distribution and Docker "
                           "container (wsl --shutdown).",
                           size_text(disks->total)));
  if (!ImGui::BeginTable("vhdx", 4, table_flags)) {
    return;
  }
  ImGui::TableSetupColumn("Disk", ImGuiTableColumnFlags_WidthStretch, 3.6F);
  ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 1.0F);
  ImGui::TableSetupColumn("In use", ImGuiTableColumnFlags_WidthStretch, 0.6F);
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.6F);
  ImGui::TableHeadersRow();
  for (const tray_model::VirtualDiskRow& disk : disks->disks) {
    ImGui::TableNextRow();
    ImGui::PushID(static_cast<int>(disk.id));
    ImGui::TableNextColumn();
    wrapped_text(color::text, disk.path);
    muted_text(disk.distribution.empty() ? disk.label : disk.label + " - " + disk.distribution);
    ImGui::TableNextColumn();
    text(size_text(disk.size));
    muted_text(size_text(disk.size_on_disk) + " on disk");
    ImGui::TableNextColumn();
    text(disk.in_use ? "yes" : "no");
    ImGui::TableNextColumn();
    action_button("Propose compaction",
                  ScreenAction{.kind = ScreenAction::Kind::compact, .ids = {disk.id}}, reachable,
                  state);
    ImGui::PopID();
  }
  ImGui::EndTable();
}

void draw_dev_drive(const DashboardModel& model, const std::chrono::time_zone& zone) {
  const auto* advice = ready(model.screens().dev_drive(), "Dev Drive");
  if (advice == nullptr) {
    return;
  }
  screen_ui::draw_analysis(advice->analysis, zone);
  if (advice->analysis.state != "ready" && advice->build.empty()) {
    return;
  }
  wrapped_text(color::muted, advice->explanation);
  text(advice->supported
           ? std::format("Windows build {} supports Dev Drives.", advice->build)
           : std::format("Windows build {} does not support Dev Drives.", advice->build));
  for (const std::string& drive : advice->dev_drives) {
    text("Dev Drive: " + drive);
  }
  text(std::format(
      "{} active repositories and {} package caches ({}) would move; a Dev Drive of "
      "{} is recommended{}.",
      advice->repositories, advice->caches, size_text(advice->total),
      size_text(advice->recommended), advice->host.empty() ? "" : ", on " + advice->host));
  for (std::size_t step = 0; step < advice->steps.size(); ++step) {
    wrapped_text(color::text, std::format("{}. {}", step + 1, advice->steps[step]));
  }
  for (const std::string& note : advice->notes) {
    wrapped_text(color::muted, note);
  }
}

void draw_costs(const DashboardModel& model) {
  const auto* costs = ready(model.screens().resident_costs(), "Background programs");
  if (costs == nullptr) {
    return;
  }
  if (costs->empty()) {
    muted_text("No program ran 24 hours or more in the last 30 days yet.");
    return;
  }
  if (!ImGui::BeginTable("costs", 6, table_flags)) {
    return;
  }
  for (const char* column : {"Program", "Hours", "CPU", "RAM (avg / peak)", "Disk", "Network"}) {
    ImGui::TableSetupColumn(column);
  }
  ImGui::TableHeadersRow();
  for (std::size_t index = 0; index < std::min(costs->size(), listed_costs); ++index) {
    const tray_model::ResidentCostRow& cost = (*costs)[index];
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    text(cost.image);
    ImGui::TableNextColumn();
    text(std::format("{:.0f}", cost.hours_running));
    ImGui::TableNextColumn();
    text(std::format("{:.0f} min", cost.cpu_seconds / 60.0));
    ImGui::TableNextColumn();
    text(size_text(cost.ram_average) + " / " + size_text(cost.ram_peak));
    ImGui::TableNextColumn();
    text(size_text(cost.disk));
    ImGui::TableNextColumn();
    text(size_text(cost.network));
  }
  ImGui::EndTable();
}

void draw_startup_item(const tray_model::StartupItemRow& item, UiState& state) {
  ImGui::TableNextRow();
  ImGui::PushID(item.key.c_str());
  ImGui::TableNextColumn();
  ImGui::BeginDisabled(!item.enabled.has_value());
  bool on = state.startup_ticked.contains(item.key);
  if (ImGui::Checkbox("##pick", &on)) {
    if (on) {
      state.startup_ticked.insert(item.key);
    } else {
      state.startup_ticked.erase(item.key);
    }
  }
  ImGui::EndDisabled();
  ImGui::TableNextColumn();
  text(item.display_name.empty() ? item.name : item.display_name);
  wrapped_text(color::muted, item.command);
  ImGui::TableNextColumn();
  text(tray_model::startup_source_text(item.source));
  muted_text(item.scope + (item.signer.empty() ? "" : ", " + item.signer));
  ImGui::TableNextColumn();
  if (!item.enabled) {
    muted_text("runs once");
  } else {
    colored_text(*item.enabled ? color::text : color::muted,
                 *item.enabled ? "enabled" : "disabled");
  }
  ImGui::TableNextColumn();
  wrapped_text(color::text, tray_model::impact_text(item.impact));
  ImGui::PopID();
}

// Proposes changing the ticked items that can change that way.
void startup_button(const std::vector<tray_model::StartupItemRow>& items, bool enable,
                    bool reachable, UiState& state) {
  const Result<std::vector<std::string>> keys =
      tray_model::startup_choice(items, state.startup_ticked, enable);
  ImGui::BeginDisabled(!reachable || !keys);
  const std::string label = std::format("Propose {} the ticked items ({})",
                                        enable ? "enabling" : "disabling", keys ? keys->size() : 0);
  if (ImGui::Button(label.c_str()) && keys) {
    state.requests.emplace_back(
        ScreenAction{.kind = ScreenAction::Kind::startup, .keys = *keys, .enable = enable});
    state.startup_ticked.clear();
  }
  ImGui::EndDisabled();
}

void draw_startup_items(const DashboardModel& model, bool reachable, UiState& state) {
  const auto* items = ready(model.screens().startup_items(), "Startup items");
  if (items == nullptr) {
    return;
  }
  startup_button(*items, false, reachable, state);
  ImGui::SameLine();
  startup_button(*items, true, reachable, state);
  if (!ImGui::BeginTable("startup", 5, table_flags)) {
    return;
  }
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.3F);
  ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch, 3.0F);
  ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch, 1.4F);
  ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 0.8F);
  ImGui::TableSetupColumn("Impact", ImGuiTableColumnFlags_WidthStretch, 2.6F);
  ImGui::TableHeadersRow();
  for (const tray_model::StartupItemRow& item : *items) {
    draw_startup_item(item, state);
  }
  ImGui::EndTable();
}

void draw_boot_times(const DashboardModel& model, const std::chrono::time_zone& zone) {
  const auto* times = ready(model.screens().boot_times(), "Boot times");
  if (times == nullptr) {
    return;
  }
  if (!times->log_error.empty()) {
    wrapped_text(color::warning, "Windows' boot log cannot be read now: " + times->log_error);
  }
  if (!times->latest_start) {
    muted_text("No boot measured yet.");
  } else {
    text(std::format("Latest boot {}: boot {}{}, logon {}{}",
                     local_date_time(zone, *times->latest_start), seconds_text(times->boot_seconds),
                     difference_text(times->boot_difference), seconds_text(times->logon_seconds),
                     difference_text(times->logon_difference)));
  }
  if (times->median_boot && times->median_logon) {
    muted_text(std::format("Median of {} boots in 30 days: boot {}, logon {}", times->median_boots,
                           seconds_text(*times->median_boot), seconds_text(*times->median_logon)));
  }
  for (const tray_model::BootContributorRow& contributor : times->contributors) {
    muted_text(
        std::format("{}: {} CPU, {} of disk{}", contributor.image,
                    seconds_text(contributor.cpu_seconds), size_text(contributor.io_bytes),
                    contributor.item.empty() ? "" : " (startup item " + contributor.item + ")"));
  }
  for (const tray_model::StartupChangeRow& change : times->changes) {
    const auto logon = [](const std::optional<double>& seconds) {
      return seconds ? seconds_text(*seconds) : std::string("not measured yet");
    };
    text(std::format("Plan {} ({}) disabled {}: logon {} before, {} after", change.plan,
                     local_date_time(zone, change.applied), ui::join(change.items, ", "),
                     logon(change.logon_before), logon(change.logon_after)));
  }
}

}  // namespace

void draw_placement(const DashboardModel& model, const std::chrono::time_zone& /*zone*/,
                    UiState& state) {
  const bool reachable = model.service_reachable();
  wrapped_text(color::muted,
               "What holds space on the system drive, and which drive the caches live on. Propose "
               "creates a plan that waits in Cleanup until you approve it. Only below 10 % free on "
               "the system drive does MyClean reduce hibernation and move the page file by "
               "itself.");
  screen_ui::request(tray_model::Screen::placement, reachable, state);
  screen_ui::draw_outcome(model);
  ImGui::SeparatorText("System settings");
  draw_system_space(model, reachable, state);
  ImGui::SeparatorText("Relocations");
  draw_relocations(model, reachable, state);
}

void draw_optimize(const DashboardModel& model, const std::chrono::time_zone& zone,
                   UiState& state) {
  const bool reachable = model.service_reachable();
  wrapped_text(color::muted,
               "Each optimization is measured before and after; a reversible one that made things "
               "worse is undone. Propose creates a plan that waits in Cleanup until you approve "
               "it.");
  screen_ui::request(tray_model::Screen::optimize, reachable, state);
  screen_ui::draw_outcome(model);
  ImGui::SeparatorText("Applied optimizations");
  draw_applied(model);
  ImGui::SeparatorText("Build and cache folders in the search index");
  draw_indexed(model, zone, reachable, state);
  ImGui::SeparatorText("WSL and Docker virtual disks");
  draw_disks(model, zone, reachable, state);
  ImGui::SeparatorText("Dev Drive");
  draw_dev_drive(model, zone);
  ImGui::SeparatorText("Background programs (30 days)");
  draw_costs(model);
}

void draw_startup(const DashboardModel& model, const std::chrono::time_zone& zone, UiState& state) {
  const bool reachable = model.service_reachable();
  wrapped_text(color::muted,
               "Tick items and propose disabling or enabling them: the plan waits in Cleanup "
               "until you approve it, and Undo restores them.");
  screen_ui::request(tray_model::Screen::startup, reachable, state);
  screen_ui::draw_outcome(model);
  ImGui::SeparatorText("Startup items");
  draw_startup_items(model, reachable, state);
  ImGui::SeparatorText("Boot and logon");
  draw_boot_times(model, zone);
}

}  // namespace myclean::tray
