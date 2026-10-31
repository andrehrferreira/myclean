#pragma once

#include <chrono>

#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/replies.hpp"

namespace myclean::tray {

enum class Section { cpu_memory, disks, volumes, processes };

// What the owner chose in the dashboard; kept between frames.
struct UiState {
  Section section = Section::cpu_memory;
  tray_model::ProcessSort sort = tray_model::ProcessSort::cpu;
};

// The dark theme of the dashboard at `scale` (1.0 at 96 DPI): colors, spacing and the font size
// of ImGui and ImPlot. Needs both contexts current; call again when the DPI changes.
void apply_style(float scale);

// Draws the whole dashboard into the current ImGui frame (between NewFrame and Render), filling
// the main viewport. Local times use `zone`. Needs an ImPlot context.
void draw_dashboard(const tray_model::DashboardModel& model, const std::chrono::time_zone& zone,
                    UiState& state);

}  // namespace myclean::tray
