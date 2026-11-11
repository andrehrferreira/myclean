#include "tray/screens_ui.hpp"

#include <chrono>
#include <utility>

#include <imgui.h>

#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/screens.hpp"
#include "tray/dashboard_ui.hpp"
#include "tray/ui_text.hpp"

namespace myclean::tray::screen_ui {

void request(tray_model::Screen screen, bool reachable, UiState& state) {
  ImGui::BeginDisabled(!reachable);
  const bool refresh = ImGui::Button("Refresh");
  ImGui::EndDisabled();
  if (refresh || (reachable && state.screens_asked.insert(screen).second)) {
    if (screen == tray_model::Screen::audit) {
      state.audit_pages.clear();  // back to the newest page
    }
    state.requests.emplace_back(tray_model::LoadScreen{.screen = screen, .refresh = refresh});
  }
}

void draw_outcome(const tray_model::DashboardModel& model) {
  if (const auto& action = model.screens().action()) {
    ui::wrapped_text(action->failed ? ui::color::bad : ui::color::good, action->text);
  }
}

void action_button(const char* label, tray_model::ScreenAction action, bool reachable,
                   UiState& state) {
  ImGui::BeginDisabled(!reachable);
  if (ImGui::SmallButton(label)) {
    state.requests.emplace_back(std::move(action));
  }
  ImGui::EndDisabled();
}

void draw_analysis(const tray_model::AnalysisState& analysis, const std::chrono::time_zone& zone) {
  if (analysis.state == "scanning") {
    ui::colored_text(ui::color::accent, "Analysing...");
  }
  if (!analysis.error.empty()) {
    ui::wrapped_text(ui::color::bad, analysis.error);
  }
  if (analysis.scanned_at) {
    ui::muted_text("As of " + ui::local_date_time(zone, *analysis.scanned_at));
  }
}

}  // namespace myclean::tray::screen_ui
