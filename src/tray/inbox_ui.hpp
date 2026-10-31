#pragma once

#include <chrono>

#include "core/tray_model/dashboard_model.hpp"
#include "tray/dashboard_ui.hpp"

// The dashboard's Cleanup section (approval inbox, approvals spec R7) and Space section (space
// spec): drawn into the current ImGui frame; the owner's clicks become requests in `state`.
namespace myclean::tray {

// Scan now and its progress, what the waiting plans would free by tier and by kind, each plan
// waiting for approval with its items, exact commands, risks and undo, approve all / selected /
// reject, the recent plans with Undo where it is possible, and the quarantine with Restore.
void draw_cleanup(const tray_model::DashboardModel& model, const std::chrono::time_zone& zone,
                  UiState& state);

// The read-only Space lists: the largest files and folders of a volume, old downloads with their
// origin, duplicate groups with the copy to keep, and installers already used.
void draw_space(const tray_model::DashboardModel& model, const std::chrono::time_zone& zone,
                UiState& state);

}  // namespace myclean::tray
