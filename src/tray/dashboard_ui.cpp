#include "tray/dashboard_ui.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>
#include <implot.h>

#include "core/model/series.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/tray_model/chart.hpp"
#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/format.hpp"
#include "core/tray_model/replies.hpp"
#include "core/tray_model/snapshot.hpp"

namespace myclean::tray {

namespace {

namespace metric = telemetry::metric;
using tray_model::ChartSeries;
using tray_model::DashboardModel;

constexpr double gib = 1024.0 * 1024.0 * 1024.0;
constexpr double mib = 1024.0 * 1024.0;
constexpr int heatmap_seconds = 120;

// The palette, as 0xRRGGBB.
namespace color {
constexpr unsigned background = 0x111418;
constexpr unsigned panel = 0x181C21;
constexpr unsigned raised = 0x20252B;
constexpr unsigned hover = 0x2A3038;
constexpr unsigned border = 0x2A2F36;
constexpr unsigned text = 0xE6E8EB;
constexpr unsigned muted = 0x8B939C;
constexpr unsigned accent = 0x4C9AFF;
constexpr unsigned good = 0x2EB872;
constexpr unsigned warning = 0xE8B21E;
constexpr unsigned bad = 0xE5484D;
}  // namespace color

ImVec4 rgb(unsigned value, float alpha = 1.0F) {
  return {static_cast<float>((value >> 16U) & 0xFFU) / 255.0F,
          static_cast<float>((value >> 8U) & 0xFFU) / 255.0F,
          static_cast<float>(value & 0xFFU) / 255.0F, alpha};
}

void text(std::string_view value) {
  ImGui::TextUnformatted(value.data(), value.data() + value.size());
}

void muted_text(std::string_view value) {
  ImGui::PushStyleColor(ImGuiCol_Text, rgb(color::muted));
  text(value);
  ImGui::PopStyleColor();
}

void colored_text(unsigned value_color, std::string_view value) {
  ImGui::PushStyleColor(ImGuiCol_Text, rgb(value_color));
  text(value);
  ImGui::PopStyleColor();
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string joined;
  for (const std::string& part : parts) {
    joined += (joined.empty() ? "" : std::string(separator)) + part;
  }
  return joined;
}

std::optional<double> value(const DashboardModel& model, std::string_view name,
                            std::string_view entity = metric::system) {
  return model.metrics().value(name, entity);
}

const ChartSeries* chart(const DashboardModel& model, std::string_view name,
                         std::string_view entity = metric::system) {
  return model.chart(SeriesKey{.metric = std::string(name), .entity = std::string(entity)});
}

// A chart's points with the values divided by `divisor`, for the plot getter.
struct Scaled {
  const ChartSeries* series = nullptr;
  double divisor = 1.0;
};

ImPlotPoint scaled_point(int index, void* data) {
  const Scaled& scaled = *static_cast<const Scaled*>(data);
  const auto at = static_cast<std::size_t>(index);
  return {scaled.series->times()[at], scaled.series->values()[at] / scaled.divisor};
}

void plot_line(const char* label, const ChartSeries* series, double divisor = 1.0) {
  if (series == nullptr || series->empty()) {
    return;
  }
  Scaled scaled{.series = series, .divisor = divisor};
  ImPlot::PlotLineG(label, scaled_point, &scaled, static_cast<int>(series->times().size()),
                    ImPlotSpec(ImPlotProp_LineWeight, 1.5F));
}

// Starts a time plot over the live window that ends at `now`. `y_max` fixes the y range from 0
// (percentages); without it the y axis fits the data.
bool begin_time_plot(const char* id, float height, double now, double window, const char* y_format,
                     std::optional<double> y_max = std::nullopt) {
  if (!ImPlot::BeginPlot(id, ImVec2(-1.0F, height),
                         ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
    return false;
  }
  ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_None,
                    y_max ? ImPlotAxisFlags_None : ImPlotAxisFlags_AutoFit);
  ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
  ImPlot::SetupAxisLimits(ImAxis_X1, now - window, now, ImPlotCond_Always);
  ImPlot::SetupAxisFormat(ImAxis_Y1, y_format);
  if (y_max) {
    ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, *y_max, ImPlotCond_Always);
  }
  ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Horizontal);
  return true;
}

float chart_height(float lines) {
  return ImGui::GetTextLineHeightWithSpacing() * lines;
}

// One headline figure: a muted caption over a large value.
void tile(const char* caption, const std::string& figure, const std::string& detail) {
  ImGui::BeginGroup();
  muted_text(caption);
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5F);
  text(figure);
  ImGui::PopFont();
  muted_text(detail);
  ImGui::EndGroup();
}

void draw_header(const DashboardModel& model, const std::chrono::time_zone& zone) {
  const std::optional<WallTime> time = model.metrics_time();
  const std::string as_of =
      time ? tray_model::format_time_of_day(
                 std::chrono::floor<std::chrono::seconds>(zone.to_local(*time)))
           : std::string(tray_model::not_available);
  if (model.service_reachable()) {
    colored_text(color::good, "Live");
    ImGui::SameLine();
    muted_text("updated " + as_of);
  } else {
    // The banner: what is missing, why, and how old the values below are.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, rgb(color::bad, 0.18F));
    ImGui::BeginChild("banner", ImVec2(0.0F, 0.0F),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    colored_text(color::bad, "Service not running");
    text("Unavailable: " + join(model.unavailable_features(), ", ") + ".");
    muted_text(time ? "The values below are not live; they are from " + as_of + "."
                    : "No data has been received yet.");
    if (!model.service_error().empty()) {
      muted_text("Reason: " + model.service_error());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
  }
  if (!model.unavailable_collectors().empty()) {
    muted_text("Not available on this machine: " + join(model.unavailable_collectors(), "; "));
  }
  ImGui::Spacing();

  if (ImGui::BeginTable("tiles", 5, ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableNextColumn();
    tile("CPU", tray_model::format_percent(value(model, metric::cpu_utility)),
         "queue " + tray_model::format_count(value(model, metric::cpu_queue)));
    const std::optional<double> total = value(model, metric::memory_total);
    const std::optional<double> available = value(model, metric::memory_available);
    ImGui::TableNextColumn();
    tile("Memory in use",
         total && available ? tray_model::format_bytes(*total - *available)
                            : std::string(tray_model::not_available),
         "of " + tray_model::format_bytes(total));
    ImGui::TableNextColumn();
    tile("Commit", tray_model::format_bytes(value(model, metric::memory_commit)),
         "limit " + tray_model::format_bytes(value(model, metric::memory_commit_limit)));
    const auto system = std::ranges::find_if(model.volumes(), &tray_model::VolumeRow::system);
    ImGui::TableNextColumn();
    if (system != model.volumes().end()) {
      tile((system->name + " free").c_str(),
           tray_model::format_bytes(static_cast<double>(system->free.count())),
           "of " + tray_model::format_bytes(static_cast<double>(system->size.count())));
    } else {
      tile("System volume free", std::string(tray_model::not_available), "");
    }
    ImGui::TableNextColumn();
    tile("Disk health", std::string(tray_model::not_available), "not reported by the service");
    ImGui::EndTable();
  }
}

// Utilization of each logical processor over the last two minutes, one row per processor.
void draw_processor_heatmap(const DashboardModel& model, double now) {
  const unsigned count = model.logical_processors();
  if (count == 0) {
    return;
  }
  const double start = now - heatmap_seconds;
  std::vector<double> cells(static_cast<std::size_t>(count) * heatmap_seconds, 0.0);
  for (unsigned row = 0; row < count; ++row) {
    const ChartSeries* series =
        chart(model, metric::cpu_utility, tray_model::processor_entity(row));
    if (series == nullptr) {
      continue;
    }
    for (std::size_t i = 0; i < series->times().size(); ++i) {
      const double column = series->times()[i] - start;
      if (column >= 0.0 && column < heatmap_seconds) {
        cells[(row * static_cast<std::size_t>(heatmap_seconds)) +
              static_cast<std::size_t>(column)] = series->values()[i];
      }
    }
  }
  const float height = std::max(chart_height(6.0F), static_cast<float>(count) * 6.0F);
  if (ImPlot::BeginPlot("##cores", ImVec2(-1.0F - (ImGui::GetFontSize() * 5.0F), height),
                        ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoLegend |
                            ImPlotFlags_NoMouseText)) {
    ImPlot::SetupAxes(nullptr, "logical processor", ImPlotAxisFlags_None,
                      ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoGridLines);
    ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
    ImPlot::SetupAxisLimits(ImAxis_X1, start, now, ImPlotCond_Always);
    ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, count, ImPlotCond_Always);
    ImPlot::PlotHeatmap("utilization", cells.data(), static_cast<int>(count), heatmap_seconds, 0.0,
                        100.0, nullptr, ImPlotPoint(start, 0.0),
                        ImPlotPoint(now, static_cast<double>(count)));
    ImPlot::EndPlot();
  }
  ImGui::SameLine();
  ImPlot::ColormapScale("##scale", 0.0, 100.0, ImVec2(ImGui::GetFontSize() * 4.0F, height),
                        "%g %%");
}

void draw_processor_frequency(const DashboardModel& model) {
  const unsigned count = model.logical_processors();
  std::vector<double> frequency(count, 0.0);
  bool any = false;
  for (unsigned index = 0; index < count; ++index) {
    const ChartSeries* series =
        chart(model, metric::cpu_frequency, tray_model::processor_entity(index));
    if (series != nullptr && !series->empty()) {
      frequency[index] = series->newest_value().value_or(0.0);
      any = true;
    }
  }
  if (!any) {
    muted_text("Per-processor frequency: " + std::string(tray_model::not_available));
    return;
  }
  if (ImPlot::BeginPlot("##frequency", ImVec2(-1.0F, chart_height(6.0F)),
                        ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoLegend)) {
    ImPlot::SetupAxes("logical processor", "% of maximum frequency");
    ImPlot::SetupAxisLimits(ImAxis_X1, -0.5, static_cast<double>(count) - 0.5, ImPlotCond_Always);
    ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, std::max(100.0, *std::ranges::max_element(frequency)),
                            ImPlotCond_Always);
    ImPlot::PlotBars("frequency", frequency.data(), static_cast<int>(count), 0.7);
    ImPlot::EndPlot();
  }
}

void draw_cpu_memory(const DashboardModel& model, double now, double window) {
  ImGui::SeparatorText("CPU");
  if (begin_time_plot("##cpu", chart_height(8.0F), now, window, "%g %%", 100.0)) {
    plot_line("utilization", chart(model, metric::cpu_utility));
    plot_line("DPC", chart(model, metric::cpu_dpc));
    plot_line("interrupts", chart(model, metric::cpu_interrupt));
    ImPlot::EndPlot();
  }
  muted_text("Utilization per logical processor, last 2 minutes");
  draw_processor_heatmap(model, now);
  draw_processor_frequency(model);

  ImGui::SeparatorText("Memory");
  if (begin_time_plot("##memory", chart_height(9.0F), now, window, "%g GB")) {
    plot_line("available", chart(model, metric::memory_available), gib);
    plot_line("commit", chart(model, metric::memory_commit), gib);
    plot_line("commit limit", chart(model, metric::memory_commit_limit), gib);
    plot_line("standby", chart(model, metric::memory_standby), gib);
    plot_line("modified", chart(model, metric::memory_modified), gib);
    plot_line("compressed", chart(model, metric::memory_compressed), gib);
    ImPlot::EndPlot();
  }
  if (begin_time_plot("##faults", chart_height(5.0F), now, window, "%g/s")) {
    plot_line("hard faults (pages read/s)", chart(model, metric::memory_hard_faults));
    ImPlot::EndPlot();
  }

  const std::vector<std::string> adapters = model.metrics().entities(metric::gpu_utilization);
  ImGui::SeparatorText("GPU");
  if (adapters.empty()) {
    muted_text(std::string(tray_model::not_available));
    return;
  }
  if (begin_time_plot("##gpu", chart_height(6.0F), now, window, "%g %%", 100.0)) {
    for (std::size_t i = 0; i < adapters.size(); ++i) {
      plot_line(std::format("adapter {}", i).c_str(),
                chart(model, metric::gpu_utilization, adapters[i]));
    }
    ImPlot::EndPlot();
  }
  for (std::size_t i = 0; i < adapters.size(); ++i) {
    muted_text(
        std::format("adapter {}: dedicated {}, shared {}", i,
                    tray_model::format_bytes(value(model, metric::gpu_dedicated, adapters[i])),
                    tray_model::format_bytes(value(model, metric::gpu_shared, adapters[i]))));
  }
}

void draw_disk(const DashboardModel& model, const std::string& disk, double now, double window) {
  const std::vector<std::string> volumes = model.volumes_on(disk);
  ImGui::PushID(disk.c_str());
  ImGui::BeginChild("disk", ImVec2(0.0F, 0.0F),
                    ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2F);
  text("Disk " + disk);
  ImGui::PopFont();
  ImGui::SameLine();
  muted_text("model " + std::string(tray_model::not_available) + "  |  volumes " +
             (volumes.empty() ? std::string("none") : join(volumes, ", ")) + "  |  health " +
             std::string(tray_model::not_available));
  muted_text(std::format(
      "read {}  write {}  |  {} IOPS  |  latency {} read, {} write  |  queue {}  |  idle {}",
      tray_model::format_rate(value(model, metric::disk_read_bytes, disk)),
      tray_model::format_rate(value(model, metric::disk_write_bytes, disk)),
      tray_model::format_count(value(model, metric::disk_iops, disk)),
      tray_model::format_milliseconds(value(model, metric::disk_read_latency, disk)),
      tray_model::format_milliseconds(value(model, metric::disk_write_latency, disk)),
      tray_model::format_count(value(model, metric::disk_queue, disk)),
      tray_model::format_percent(value(model, metric::disk_idle, disk))));
  if (ImGui::BeginTable("charts", 4, ImGuiTableFlags_SizingStretchSame)) {
    const float height = chart_height(6.0F);
    ImGui::TableNextColumn();
    if (begin_time_plot("Throughput##t", height, now, window, "%g MB/s")) {
      plot_line("read", chart(model, metric::disk_read_bytes, disk), mib);
      plot_line("write", chart(model, metric::disk_write_bytes, disk), mib);
      ImPlot::EndPlot();
    }
    ImGui::TableNextColumn();
    if (begin_time_plot("IOPS##i", height, now, window, "%g")) {
      plot_line("transfers/s", chart(model, metric::disk_iops, disk));
      ImPlot::EndPlot();
    }
    ImGui::TableNextColumn();
    if (begin_time_plot("Latency##l", height, now, window, "%g ms")) {
      plot_line("read", chart(model, metric::disk_read_latency, disk));
      plot_line("write", chart(model, metric::disk_write_latency, disk));
      ImPlot::EndPlot();
    }
    ImGui::TableNextColumn();
    if (begin_time_plot("Queue and idle##q", height, now, window, "%g %%", 100.0)) {
      ImPlot::SetupAxis(ImAxis_Y2, nullptr, ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
      plot_line("idle", chart(model, metric::disk_idle, disk));
      ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
      plot_line("queue", chart(model, metric::disk_queue, disk));
      ImPlot::EndPlot();
    }
    ImGui::EndTable();
  }
  ImGui::EndChild();
  ImGui::PopID();
}

void draw_disks(const DashboardModel& model, double now, double window) {
  const std::vector<std::string> disks = model.disks();
  if (disks.empty()) {
    muted_text("No physical disk has been sampled.");
    return;
  }
  muted_text("Health, temperature, wear and error counters: " +
             std::string(tray_model::not_available) + " (not reported by the service yet).");
  for (const std::string& disk : disks) {
    draw_disk(model, disk, now, window);
  }
}

std::string projection(const tray_model::VolumeRow& volume) {
  switch (volume.fill) {
    case tray_model::FillState::filling:
      return volume.days_until_full
                 ? tray_model::format_days(*volume.days_until_full) + " until full"
                 : "filling";
    case tray_model::FillState::not_filling:
      return "not filling";
    case tray_model::FillState::insufficient_data:
      break;
  }
  return "trend: not enough history yet";
}

void draw_volumes(const DashboardModel& model) {
  if (model.volumes().empty()) {
    muted_text("No volume has been sampled.");
    return;
  }
  if (!ImGui::BeginTable("volumes", 4,
                         ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                             ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
    return;
  }
  ImGui::TableSetupColumn("Volume", ImGuiTableColumnFlags_WidthStretch, 1.0F);
  ImGui::TableSetupColumn("Disks", ImGuiTableColumnFlags_WidthStretch, 0.6F);
  ImGui::TableSetupColumn("Space", ImGuiTableColumnFlags_WidthStretch, 4.0F);
  ImGui::TableSetupColumn("Projection", ImGuiTableColumnFlags_WidthStretch, 1.6F);
  ImGui::TableHeadersRow();
  for (const tray_model::VolumeRow& volume : model.volumes()) {
    const auto size = static_cast<double>(volume.size.count());
    const auto free = static_cast<double>(volume.free.count());
    const double free_fraction = size > 0.0 ? free / size : 0.0;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    text(volume.name + (volume.system ? "  (Windows)" : ""));
    ImGui::TableNextColumn();
    muted_text(volume.disks.empty() ? std::string(tray_model::not_available)
                                    : join(volume.disks, ", "));
    ImGui::TableNextColumn();
    unsigned bar = color::accent;
    if (free_fraction < 0.10) {
      bar = color::bad;
    } else if (free_fraction < 0.20) {
      bar = color::warning;
    }
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, rgb(bar));
    const std::string overlay =
        std::format("{} free of {}  ({} used)", tray_model::format_bytes(free),
                    tray_model::format_bytes(size), tray_model::format_bytes(size - free));
    ImGui::ProgressBar(static_cast<float>(1.0 - free_fraction), ImVec2(-1.0F, 0.0F),
                       overlay.c_str());
    ImGui::PopStyleColor();
    ImGui::TableNextColumn();
    text(projection(volume));
  }
  ImGui::EndTable();
}

void draw_process_table(const std::vector<tray_model::ProcessTreeRow>& trees) {
  if (!ImGui::BeginTable("trees", 6,
                         ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                             ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
    return;
  }
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("Process tree", ImGuiTableColumnFlags_WidthStretch, 3.0F);
  ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthStretch, 0.8F);
  ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthStretch, 0.8F);
  ImGui::TableSetupColumn("Private working set", ImGuiTableColumnFlags_WidthStretch, 1.4F);
  ImGui::TableSetupColumn("Private bytes", ImGuiTableColumnFlags_WidthStretch, 1.2F);
  ImGui::TableSetupColumn("Trust", ImGuiTableColumnFlags_WidthStretch, 0.8F);
  ImGui::TableHeadersRow();
  const auto columns = [](const tray_model::ProcessRow& row) {
    ImGui::TableNextColumn();
    text(std::to_string(row.pid));
    ImGui::TableNextColumn();
    text(tray_model::format_percent(row.cpu_percent));
    ImGui::TableNextColumn();
    text(tray_model::format_bytes(static_cast<double>(row.private_working_set.count())));
    ImGui::TableNextColumn();
    text(tray_model::format_bytes(static_cast<double>(row.private_bytes.count())));
  };
  for (const tray_model::ProcessTreeRow& tree : trees) {
    const tray_model::ProcessRow& root = tree.members.front();
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(static_cast<int>(root.pid));
    const bool single = tree.members.size() == 1;
    const std::string label =
        single ? root.image : std::format("{}  ({} processes)", root.image, tree.members.size());
    const bool open = ImGui::TreeNodeEx(
        label.c_str(), ImGuiTreeNodeFlags_SpanAllColumns |
                           (single ? ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen
                                   : ImGuiTreeNodeFlags_None));
    columns(tray_model::ProcessRow{.pid = root.pid,
                                   .image = root.image,
                                   .cpu_percent = tree.cpu_percent,
                                   .private_working_set = tree.private_working_set,
                                   .private_bytes = tree.private_bytes});
    ImGui::TableNextColumn();
    muted_text(tray_model::not_available);
    if (open && !single) {
      for (const tray_model::ProcessRow& member : tree.members) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(member.pid));
        ImGui::TreeNodeEx(member.image.c_str(), ImGuiTreeNodeFlags_Leaf |
                                                    ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                    ImGuiTreeNodeFlags_SpanAllColumns);
        ImGui::PopID();
        columns(member);
        ImGui::TableNextColumn();
      }
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
  ImGui::EndTable();
}

void draw_processes(const DashboardModel& model, UiState& state) {
  if (ImGui::RadioButton("By CPU", state.sort == tray_model::ProcessSort::cpu)) {
    state.sort = tray_model::ProcessSort::cpu;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("By memory", state.sort == tray_model::ProcessSort::memory)) {
    state.sort = tray_model::ProcessSort::memory;
  }
  ImGui::SameLine();
  muted_text("CPU is a share of all logical processors. Trust flags: " +
             std::string(tray_model::not_available) + " (not reported by the service yet).");
  const auto& trees =
      state.sort == tray_model::ProcessSort::cpu ? model.top_by_cpu() : model.top_by_memory();
  if (!trees) {
    muted_text("Process trees: " + std::string(tray_model::not_available));
    return;
  }
  draw_process_table(*trees);
}

}  // namespace

void apply_style(float scale) {
  ImGuiStyle& style = ImGui::GetStyle();
  style = ImGuiStyle();
  ImGui::StyleColorsDark(&style);
  style.WindowRounding = 0.0F;
  style.ChildRounding = 6.0F;
  style.FrameRounding = 4.0F;
  style.GrabRounding = 4.0F;
  style.TabRounding = 4.0F;
  style.WindowPadding = ImVec2(14.0F, 12.0F);
  style.FramePadding = ImVec2(8.0F, 4.0F);
  style.ItemSpacing = ImVec2(10.0F, 7.0F);
  style.CellPadding = ImVec2(8.0F, 4.0F);
  style.WindowBorderSize = 0.0F;
  style.ChildBorderSize = 1.0F;
  style.FontSizeBase = 16.0F;
  style.FontScaleDpi = scale;
  style.ScaleAllSizes(scale);

  const auto set = [&style](ImGuiCol slot, unsigned value, float alpha = 1.0F) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): indexed by ImGuiCol
    style.Colors[slot] = rgb(value, alpha);
  };
  set(ImGuiCol_Text, color::text);
  set(ImGuiCol_TextDisabled, color::muted);
  set(ImGuiCol_WindowBg, color::background);
  set(ImGuiCol_ChildBg, color::panel);
  set(ImGuiCol_PopupBg, color::panel);
  set(ImGuiCol_Border, color::border);
  set(ImGuiCol_FrameBg, color::raised);
  set(ImGuiCol_FrameBgHovered, color::hover);
  set(ImGuiCol_FrameBgActive, color::hover);
  set(ImGuiCol_Header, color::raised);
  set(ImGuiCol_HeaderHovered, color::hover);
  set(ImGuiCol_HeaderActive, color::hover);
  set(ImGuiCol_Tab, color::panel);
  set(ImGuiCol_TabHovered, color::hover);
  set(ImGuiCol_TabSelected, color::raised);
  set(ImGuiCol_TabSelectedOverline, color::accent);
  set(ImGuiCol_TableHeaderBg, color::raised);
  set(ImGuiCol_TableRowBg, color::panel, 0.0F);
  set(ImGuiCol_TableRowBgAlt, color::raised, 0.45F);
  set(ImGuiCol_TableBorderLight, color::border);
  set(ImGuiCol_Separator, color::border);
  set(ImGuiCol_CheckMark, color::accent);
  set(ImGuiCol_PlotHistogram, color::accent);
  set(ImGuiCol_ScrollbarBg, color::background);

  ImPlot::StyleColorsDark();
  ImPlotStyle& plot = ImPlot::GetStyle();
  plot.UseLocalTime = true;
  plot.Use24HourClock = true;
  plot.Colors[ImPlotCol_PlotBg] = rgb(color::panel);
  plot.Colors[ImPlotCol_FrameBg] = rgb(color::panel, 0.0F);
  plot.Colors[ImPlotCol_PlotBorder] = rgb(color::border);
  plot.Colormap = ImPlotColormap_Viridis;
}

void draw_dashboard(const DashboardModel& model, const std::chrono::time_zone& zone,
                    UiState& state) {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::Begin("MyClean", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
  draw_header(model, zone);

  // The charts end at the newest sample, so they stop advancing when the service stops.
  const double now = model.metrics_time() ? tray_model::chart_time(*model.metrics_time()) : 0.0;
  const double window = std::chrono::duration<double>(model.window()).count();
  // A navigation column on the left, the chosen section on the right.
  static constexpr std::array<std::pair<Section, const char*>, 4> sections = {{
      {Section::cpu_memory, "CPU and memory"},
      {Section::disks, "Disks"},
      {Section::volumes, "Volumes"},
      {Section::processes, "Processes"},
  }};
  ImGui::BeginChild("navigation", ImVec2(ImGui::GetFontSize() * 10.0F, 0.0F),
                    ImGuiChildFlags_Borders);
  for (const auto& [section, label] : sections) {
    if (ImGui::Selectable(label, state.section == section)) {
      state.section = section;
    }
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("section", ImVec2(0.0F, 0.0F), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground);
  switch (state.section) {
    case Section::cpu_memory:
      draw_cpu_memory(model, now, window);
      break;
    case Section::disks:
      draw_disks(model, now, window);
      break;
    case Section::volumes:
      draw_volumes(model);
      break;
    case Section::processes:
      draw_processes(model, state);
      break;
  }
  ImGui::EndChild();
  ImGui::End();
}

}  // namespace myclean::tray
