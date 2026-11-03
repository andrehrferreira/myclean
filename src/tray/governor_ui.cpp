#include "tray/governor_ui.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <imgui.h>

#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/format.hpp"
#include "core/tray_model/governor.hpp"
#include "core/tray_model/poller.hpp"
#include "tray/dashboard_ui.hpp"
#include "tray/ui_text.hpp"

namespace myclean::tray {

namespace {

namespace color = ui::color;
using tray_model::GovernorStatus;
using ui::colored_text;
using ui::join;
using ui::local_date_time;
using ui::muted_text;
using ui::text;
using ui::wrapped_text;

constexpr ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                        ImGuiTableFlags_SizingStretchProp |
                                        ImGuiTableFlags_PadOuterX;
constexpr int max_pause_minutes = 1440;

std::string_view step_text(std::string_view step) {
  if (step == "priority") {
    return "lower priority";
  }
  if (step == "ecoqos") {
    return "EcoQoS";
  }
  if (step == "cap_job") {
    return "CPU and memory cap";
  }
  if (step == "suspend") {
    return "suspended";
  }
  if (step == "kill") {
    return "ended";
  }
  return step;
}

void draw_state(const GovernorStatus& status) {
  if (status.mode == "observe") {
    wrapped_text(color::accent,
                 "Observe mode: the governor records what it would do and changes nothing. Set "
                 "mode = \"act\" in governor.toml to let it act.");
  } else {
    colored_text(color::good, "The governor acts on pressure.");
  }
  if (status.pressure) {
    colored_text(color::warning, "Under pressure: " + join(status.reasons, ", ") +
                                     (status.clearing ? " (clearing)" : ""));
  } else {
    muted_text("No pressure.");
  }
}

void draw_pause(const GovernorStatus& status, const std::chrono::time_zone& zone, bool reachable,
                UiState& state) {
  ImGui::BeginDisabled(!reachable);
  if (status.paused_until) {
    colored_text(color::warning, "Paused until " + local_date_time(zone, *status.paused_until));
    ImGui::SameLine();
    if (ImGui::Button("End the pause")) {
      state.requests.emplace_back(tray_model::PauseGovernor{.minutes = std::chrono::minutes{0}});
    }
  } else {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0F);
    ImGui::InputInt("minutes##pause", &state.pause_minutes, 15, 60);
    state.pause_minutes = std::clamp(state.pause_minutes, 1, max_pause_minutes);
    ImGui::SameLine();
    if (ImGui::Button("Pause governor")) {
      state.requests.emplace_back(
          tray_model::PauseGovernor{.minutes = std::chrono::minutes{state.pause_minutes}});
    }
    ImGui::SameLine();
    muted_text("Rolls back every step and resumes every suspended tree.");
  }
  ImGui::EndDisabled();
}

void draw_game_mode(const GovernorStatus& status, bool reachable, UiState& state) {
  text(std::format("Game mode {}: build jobs capped at {} % CPU.", status.game_mode ? "on" : "off",
                   status.build_cpu_percent));
  ImGui::BeginDisabled(!reachable);
  static constexpr std::array<std::pair<const char*, const char*>, 3> choices = {{
      {"auto", "Automatic"},
      {"on", "Always on"},
      {"off", "Off"},
  }};
  for (const auto& [value, label] : choices) {
    ImGui::SameLine();
    if (ImGui::RadioButton(label, status.game_override == value) && status.game_override != value) {
      state.requests.emplace_back(tray_model::SetGameMode{.value = value});
    }
  }
  ImGui::EndDisabled();
  if (status.games.empty()) {
    muted_text("No game is running.");
    return;
  }
  for (const tray_model::GovernorGame& game : status.games) {
    muted_text(std::format("Game: {} ({}); it never receives an action.", game.image, game.reason));
  }
}

void draw_choices(const tray_model::GovernorNotification& notification, bool reachable,
                  UiState& state) {
  const auto choose = [&](std::string_view choice) {
    state.requests.emplace_back(
        tray_model::ChooseForTree{.root = notification.root, .choice = std::string(choice)});
  };
  ImGui::BeginDisabled(!reachable);
  if (state.confirm_kill == notification.root) {
    colored_text(color::warning, std::format("End all {} processes?", notification.members));
    ImGui::SameLine();
    if (ImGui::SmallButton("Kill")) {
      choose("kill");
      state.confirm_kill.reset();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Cancel")) {
      state.confirm_kill.reset();
    }
    ImGui::EndDisabled();
    return;
  }
  ImGui::BeginDisabled(notification.step != "suspend");
  if (ImGui::SmallButton("Resume")) {
    choose("resume");
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::SmallButton("Kill...")) {
    state.confirm_kill = notification.root;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Allow once")) {
    choose("allow_once");
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Always allow")) {
    choose("always_allow");
  }
  ImGui::EndDisabled();
}

void draw_notifications(const GovernorStatus& status, const std::chrono::time_zone& zone,
                        bool reachable, UiState& state) {
  if (status.notifications.empty()) {
    muted_text("Nothing waits for you.");
    return;
  }
  if (!ImGui::BeginTable("notifications", 4, table_flags)) {
    return;
  }
  ImGui::TableSetupColumn("When", ImGuiTableColumnFlags_WidthStretch, 1.2F);
  ImGui::TableSetupColumn("Program", ImGuiTableColumnFlags_WidthStretch, 2.0F);
  ImGui::TableSetupColumn("Step", ImGuiTableColumnFlags_WidthStretch, 1.6F);
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 3.4F);
  ImGui::TableHeadersRow();
  for (const tray_model::GovernorNotification& notification : status.notifications) {
    ImGui::PushID(static_cast<int>(notification.id));
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    muted_text(local_date_time(zone, notification.time));
    ImGui::TableNextColumn();
    text(notification.image);
    muted_text(
        std::format("{} processes, class {}", notification.members, notification.process_class));
    ImGui::TableNextColumn();
    colored_text(notification.step == "suspend" || notification.step == "kill" ? color::warning
                                                                               : color::text,
                 step_text(notification.step));
    muted_text("reason " + notification.reason);
    ImGui::TableNextColumn();
    draw_choices(notification, reachable, state);
    ImGui::PopID();
  }
  ImGui::EndTable();
}

void draw_trees(const GovernorStatus& status) {
  if (status.trees.empty()) {
    muted_text("None.");
    return;
  }
  if (!ImGui::BeginTable("trees", 3, table_flags)) {
    return;
  }
  ImGui::TableSetupColumn("Program", ImGuiTableColumnFlags_WidthStretch, 2.0F);
  ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch, 1.0F);
  ImGui::TableSetupColumn("Step", ImGuiTableColumnFlags_WidthStretch, 1.5F);
  ImGui::TableHeadersRow();
  for (const tray_model::GovernorTree& tree : status.trees) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    text(std::format("{} (pid {})", tree.image, tree.root.pid));
    ImGui::TableNextColumn();
    text(tree.process_class);
    ImGui::TableNextColumn();
    text(step_text(tree.step));
  }
  ImGui::EndTable();
}

void draw_events(const GovernorStatus& status, const std::chrono::time_zone& zone) {
  if (status.events.empty()) {
    muted_text("None yet.");
    return;
  }
  if (!ImGui::BeginTable("events", 4, table_flags)) {
    return;
  }
  ImGui::TableSetupColumn("When", ImGuiTableColumnFlags_WidthStretch, 1.2F);
  ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthStretch, 1.4F);
  ImGui::TableSetupColumn("Program", ImGuiTableColumnFlags_WidthStretch, 1.6F);
  ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch, 4.0F);
  ImGui::TableHeadersRow();
  for (const tray_model::GovernorEventRow& event : status.events) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    muted_text(local_date_time(zone, event.time));
    ImGui::TableNextColumn();
    text(event.step ? std::format("{} {}", event.kind, *event.step) : event.kind);
    muted_text(event.actor + ", " + event.reason);
    ImGui::TableNextColumn();
    text(event.image.empty() ? std::string(tray_model::not_available) : event.image);
    ImGui::TableNextColumn();
    wrapped_text(color::muted, event.detail);
  }
  ImGui::EndTable();
}

}  // namespace

void draw_governor(const tray_model::DashboardModel& model, const std::chrono::time_zone& zone,
                   UiState& state) {
  const auto& status = model.governor();
  if (!status) {
    muted_text("Governor: " + std::string(tray_model::not_available) +
               " (not reported by the service).");
    return;
  }
  const bool reachable = model.service_reachable();
  draw_state(*status);
  if (!model.governor_action_error().empty()) {
    wrapped_text(color::bad, model.governor_action_error());
  }
  draw_pause(*status, zone, reachable, state);
  draw_game_mode(*status, reachable, state);
  ImGui::SeparatorText("Waiting for you");
  draw_notifications(*status, zone, reachable, state);
  ImGui::SeparatorText("Trees with a step in effect");
  draw_trees(*status);
  ImGui::SeparatorText("Recent decisions");
  draw_events(*status, zone);
}

}  // namespace myclean::tray
