#pragma once

#include <chrono>

#include "core/tray_model/dashboard_model.hpp"
#include "tray/dashboard_ui.hpp"

// The dashboard's Governor section (governor spec): drawn into the current ImGui frame; the
// owner's clicks become requests in `state`.
namespace myclean::tray {

// The governor's mode and pressure state, Pause for N minutes (or end the pause), the game-mode
// override auto / on / off with the detected games, every notification waiting for the owner with
// Resume, Kill (confirmed), Allow once and Always allow, the trees with a step in effect and the
// recent decisions.
void draw_governor(const tray_model::DashboardModel& model, const std::chrono::time_zone& zone,
                   UiState& state);

}  // namespace myclean::tray
