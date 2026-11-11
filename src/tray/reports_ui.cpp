#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/reports.hpp"
#include "core/tray_model/screens.hpp"
#include "tray/dashboard_ui.hpp"
#include "tray/screens_ui.hpp"
#include "tray/ui_text.hpp"

// The Reports screen (screens_ui.hpp).
namespace myclean::tray {

namespace {

namespace color = ui::color;
using screen_ui::ready;
using screen_ui::table_flags;
using tray_model::DashboardModel;
using ui::join;
using ui::local_date_time;
using ui::muted_text;
using ui::text;
using ui::wrapped_text;

// A combo over the snapshot months of `reports`; `chosen` is the month picked.
void snapshot_combo(const char* label, const std::vector<tray_model::ReportRow>& reports,
                    std::string& chosen) {
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0F);
  if (ImGui::BeginCombo(label, chosen.c_str())) {
    for (const tray_model::ReportRow& report : reports) {
      if (!report.snapshot.empty() &&
          ImGui::Selectable(report.snapshot.c_str(), report.snapshot == chosen)) {
        chosen = report.snapshot;
      }
    }
    ImGui::EndCombo();
  }
}

void draw_list(const std::vector<tray_model::ReportRow>& reports,
               const std::chrono::time_zone& zone, bool reachable, UiState& state) {
  if (!ImGui::BeginTable("reports", 4, table_flags)) {
    return;
  }
  ImGui::TableSetupColumn("Month", ImGuiTableColumnFlags_WidthStretch, 0.8F);
  ImGui::TableSetupColumn("Made", ImGuiTableColumnFlags_WidthStretch, 1.4F);
  ImGui::TableSetupColumn("Compared with", ImGuiTableColumnFlags_WidthStretch, 1.4F);
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.8F);
  ImGui::TableHeadersRow();
  for (const tray_model::ReportRow& report : reports) {
    ImGui::TableNextRow();
    ImGui::PushID(report.month.c_str());
    ImGui::TableNextColumn();
    text(report.month);
    ImGui::TableNextColumn();
    muted_text(report.generated_at ? local_date_time(zone, *report.generated_at) : "");
    ImGui::TableNextColumn();
    text(report.first_report ? std::string("(first report)") : report.against);
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(!reachable);
    if (ImGui::SmallButton("Open")) {
      state.requests.emplace_back(
          tray_model::LoadScreen{.screen = tray_model::Screen::reports, .month = report.month});
    }
    ImGui::EndDisabled();
    ImGui::PopID();
  }
  ImGui::EndTable();
}

void draw_compare(const std::vector<tray_model::ReportRow>& reports, bool reachable,
                  UiState& state) {
  snapshot_combo("from##compare", reports, state.compare_from);
  ImGui::SameLine();
  snapshot_combo("to##compare", reports, state.compare_to);
  ImGui::SameLine();
  ImGui::BeginDisabled(!reachable || state.compare_from.empty() || state.compare_to.empty());
  if (ImGui::Button("Compare")) {
    state.requests.emplace_back(tray_model::LoadScreen{.screen = tray_model::Screen::reports,
                                                       .month = state.compare_from,
                                                       .other = state.compare_to});
  }
  ImGui::EndDisabled();
}

void draw_text(const DashboardModel& model) {
  const auto& shown = model.screens().report();
  if (!shown) {
    return;
  }
  const auto* report = ready(shown, "Report");
  if (report == nullptr) {
    return;
  }
  ImGui::SeparatorText(report->title.c_str());
  if (!report->found) {
    wrapped_text(color::warning, report->message);
    if (!report->available.empty()) {
      muted_text("Available: " + join(report->available, ", "));
    }
    return;
  }
  ImGui::BeginChild("report_text", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders);
  wrapped_text(color::text, report->markdown);
  ImGui::EndChild();
}

}  // namespace

void draw_reports(const DashboardModel& model, const std::chrono::time_zone& zone, UiState& state) {
  const bool reachable = model.service_reachable();
  wrapped_text(color::muted,
               "The monthly reports MyClean writes on the first day of each month (or at the next "
               "start), each compared with the month before. Open one to read it here.");
  screen_ui::request(tray_model::Screen::reports, reachable, state);
  const auto* reports = ready(model.screens().reports(), "Reports");
  if (reports == nullptr) {
    return;
  }
  if (reports->empty()) {
    muted_text("No monthly report has been made yet.");
    return;
  }
  draw_list(*reports, zone, reachable, state);
  draw_compare(*reports, reachable, state);
  draw_text(model);
}

}  // namespace myclean::tray
