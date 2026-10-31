#include "tray/dashboard_ui.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <utility>

#include <gtest/gtest.h>
#include <imgui.h>
#include <implot.h>

#include "../tray_model/tray_model_test_support.hpp"
#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/poller.hpp"
#include "core/tray_model/replies.hpp"

namespace myclean::tray {
namespace {

using tray_model::from_unix_seconds;

constexpr std::int64_t start = 1'000'000;
constexpr std::int64_t seconds = 600;

// Dear ImGui and ImPlot without a window or a renderer: frames are built into draw lists that
// nothing displays.
class HeadlessUi {
 public:
  HeadlessUi() : imgui_(ImGui::CreateContext()), implot_(ImPlot::CreateContext()) {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0F, 880.0F);
    io.DeltaTime = 1.0F / 60.0F;
    // Font textures are queued for a renderer that never runs.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  }
  HeadlessUi(const HeadlessUi&) = delete;
  HeadlessUi& operator=(const HeadlessUi&) = delete;
  HeadlessUi(HeadlessUi&&) = delete;
  HeadlessUi& operator=(HeadlessUi&&) = delete;
  ~HeadlessUi() {
    ImPlot::DestroyContext(implot_);
    ImGui::DestroyContext(imgui_);
  }

 private:
  ImGuiContext* imgui_;
  ImPlotContext* implot_;
};

// Draws the dashboard for a few frames in the current headless context, as the window does after
// a change; returns the vertex count of the last frame.
int render(const tray_model::DashboardModel& model, UiState& state) {
  int vertices = 0;
  for (int frame = 0; frame < 3; ++frame) {
    ImGui::NewFrame();
    draw_dashboard(model, *std::chrono::locate_zone("UTC"), state);
    ImGui::Render();
    vertices = ImGui::GetDrawData()->TotalVtxCount;
  }
  return vertices;
}

// Ten minutes of live data, the per-processor history and the top process trees.
tray_model::DashboardModel full_model() {
  tray_model::DashboardModel model;
  tray_model::testing::Json reply = tray_model::testing::latest_json(start);
  for (std::int64_t second = start - seconds; second <= start; ++second) {
    reply["time"] = second;
    Result<tray_model::LatestReply> latest = tray_model::parse_metrics_latest(reply);
    tray_model::PollResult result;
    result.time = from_unix_seconds(second);
    result.reachable = true;
    result.latest = std::move(*latest);
    model.apply(std::move(result));
  }
  tray_model::PollResult open;
  open.time = from_unix_seconds(start);
  open.reachable = true;
  for (unsigned core = 0; core < 4; ++core) {
    for (const char* metric : {"cpu.utility", "cpu.frequency"}) {
      const Result<Series> series = tray_model::parse_metrics_query(tray_model::testing::query_json(
          metric, tray_model::processor_entity(core), start - 120, start + 1, 25.0 * core));
      open.history.push_back(*series);
    }
  }
  open.top_by_cpu = tray_model::parse_processes_top(tray_model::testing::top_json()).value();
  open.top_by_memory = open.top_by_cpu;
  model.apply(std::move(open));
  return model;
}

constexpr std::array<Section, 4> all_sections = {Section::cpu_memory, Section::disks,
                                                 Section::volumes, Section::processes};

TEST(DashboardUiTest, DrawsEverySectionWithLiveData) {
  const HeadlessUi ui;
  apply_style(1.0F);
  const tray_model::DashboardModel model = full_model();
  for (const Section section : all_sections) {
    UiState state{.section = section, .sort = tray_model::ProcessSort::cpu};
    EXPECT_GT(render(model, state), 0) << static_cast<int>(section);
    EXPECT_EQ(state.section, section);
  }
  UiState by_memory{.section = Section::processes, .sort = tray_model::ProcessSort::memory};
  EXPECT_GT(render(model, by_memory), 0);
}

TEST(DashboardUiTest, DrawsTheBannerAndTheLastValuesWhenTheServiceStops) {
  const HeadlessUi ui;
  apply_style(1.0F);
  tray_model::DashboardModel model = full_model();
  tray_model::PollResult down;
  down.time = from_unix_seconds(start + 5);
  down.error = "the service is not running";
  model.apply(std::move(down));
  for (const Section section : all_sections) {
    UiState state{.section = section, .sort = tray_model::ProcessSort::cpu};
    EXPECT_GT(render(model, state), 0);
  }
}

TEST(DashboardUiTest, DrawsBeforeAnyDataArrived) {
  const HeadlessUi ui;
  apply_style(1.0F);
  const tray_model::DashboardModel model;
  for (const Section section : all_sections) {
    UiState state{.section = section, .sort = tray_model::ProcessSort::cpu};
    EXPECT_GT(render(model, state), 0);
  }
}

TEST(DashboardUiTest, StyleScalesWithTheMonitorDpi) {
  const HeadlessUi ui;
  apply_style(1.0F);
  const float padding = ImGui::GetStyle().WindowPadding.x;
  apply_style(2.0F);
  EXPECT_FLOAT_EQ(ImGui::GetStyle().WindowPadding.x, padding * 2.0F);
  EXPECT_FLOAT_EQ(ImGui::GetStyle().FontScaleDpi, 2.0F);
  EXPECT_TRUE(ImPlot::GetStyle().UseLocalTime);
  const tray_model::DashboardModel model = full_model();
  UiState state;
  EXPECT_GT(render(model, state), 0);
}

}  // namespace
}  // namespace myclean::tray
