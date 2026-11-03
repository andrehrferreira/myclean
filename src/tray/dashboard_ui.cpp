#include "tray/dashboard_ui.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>
#include <implot.h>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/tray_model/chart.hpp"
#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/format.hpp"
#include "core/tray_model/poller.hpp"
#include "core/tray_model/replies.hpp"
#include "core/tray_model/snapshot.hpp"
#include "core/tray_model/tray_state.hpp"
#include "tray/governor_ui.hpp"
#include "tray/inbox_ui.hpp"
#include "tray/ui_text.hpp"

namespace myclean::tray {

namespace {

namespace metric = telemetry::metric;
using tray_model::ChartSeries;
using tray_model::DashboardModel;
using tray_model::Range;
using ui::colored_text;
using ui::join;
using ui::local_date_time;
using ui::muted_text;
using ui::rgb;
using ui::text;
using ui::wrapped_text;
namespace color = ui::color;

constexpr double gib = 1024.0 * 1024.0 * 1024.0;
constexpr double mib = 1024.0 * 1024.0;
constexpr int heatmap_seconds = 120;

// "clipboard_held_open" -> "Clipboard held open".
std::string humanize(std::string_view name) {
  std::string result(name);
  std::ranges::replace(result, '_', ' ');
  if (!result.empty() && result.front() >= 'a' && result.front() <= 'z') {
    result.front() = static_cast<char>(result.front() - 'a' + 'A');
  }
  return result;
}

std::optional<double> value(const DashboardModel& model, std::string_view name,
                            std::string_view entity = metric::system) {
  return model.metrics().value(name, entity);
}

const ChartSeries* live_chart(const DashboardModel& model, std::string_view name,
                              std::string_view entity) {
  return model.chart(SeriesKey{.metric = std::string(name), .entity = std::string(entity)});
}

// What the time charts show: the live window, or the past range the owner chose.
struct ChartView {
  const DashboardModel* model = nullptr;
  Range range = Range::live;
  bool ready = false;  // false while the chosen past range has not arrived
  double now = 0.0;    // the right edge
  double window = 0.0;

  [[nodiscard]] const ChartSeries* get(std::string_view name,
                                       std::string_view entity = metric::system) const {
    if (!ready) {
      return nullptr;
    }
    const SeriesKey key{.metric = std::string(name), .entity = std::string(entity)};
    return range == Range::live ? model->chart(key) : model->range_chart(key);
  }
};

ChartView chart_view(const DashboardModel& model, Range range) {
  ChartView view{
      .model = &model,
      .range = range,
      .ready = range == Range::live || model.range() == range,
      .now = 0.0,
      .window =
          std::chrono::duration<double>(tray_model::range_length(range, model.window())).count()};
  // The live charts end at the newest sample, so they stop advancing when the service stops.
  const std::optional<WallTime> end =
      range == Range::live || !view.ready ? model.metrics_time() : model.range_time();
  view.now = end ? tray_model::chart_time(*end) : 0.0;
  return view;
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

// Starts a time plot over the view's span. `y_max` fixes the y range from 0 (percentages);
// without it the y axis fits the data.
bool begin_time_plot(const char* id, float height, const ChartView& view, const char* y_format,
                     std::optional<double> y_max = std::nullopt) {
  if (!ImPlot::BeginPlot(id, ImVec2(-1.0F, height),
                         ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
    return false;
  }
  ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_None,
                    y_max ? ImPlotAxisFlags_None : ImPlotAxisFlags_AutoFit);
  ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
  ImPlot::SetupAxisLimits(ImAxis_X1, view.now - view.window, view.now, ImPlotCond_Always);
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

// Live, or one of the stored ranges served by the history query.
void draw_range_selector(const ChartView& view, UiState& state) {
  static constexpr std::array<std::pair<Range, const char*>, 4> ranges = {{
      {Range::live, "Live (10 min)"},
      {Range::hour, "1 h"},
      {Range::day, "24 h"},
      {Range::month, "30 d"},
  }};
  for (const auto& [range, label] : ranges) {
    if (range != Range::live) {
      ImGui::SameLine();
    }
    if (ImGui::RadioButton(label, state.range == range) && state.range != range) {
      state.range = range;
      state.requests.emplace_back(tray_model::SelectRange{.range = range});
    }
  }
  ImGui::SameLine();
  if (!view.ready) {
    muted_text("loading the stored history...");
  } else if (view.range != Range::live) {
    muted_text("stored averages, refreshed every minute");
  } else {
    text("");
  }
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

void draw_tiles(const DashboardModel& model) {
  if (!ImGui::BeginTable("tiles", 6, ImGuiTableFlags_SizingStretchSame)) {
    return;
  }
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
  const auto& drives = model.drives();
  const std::optional<std::string> worst =
      drives ? tray_model::worst_drive_health(*drives) : std::nullopt;
  ImGui::TableNextColumn();
  tile("Disk health", worst.value_or(std::string(tray_model::not_available)),
       drives ? std::format("{} drives", drives->size()) : "not reported by the service");
  const auto& badge = model.badge();
  ImGui::TableNextColumn();
  tile("Alerts", badge ? std::to_string(badge->active) : std::string(tray_model::not_available),
       badge && badge->worst ? "worst " + std::string(tray_model::to_string(*badge->worst))
                             : "none active");
  ImGui::EndTable();
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
  draw_tiles(model);
}

// Utilization of each logical processor over the last two minutes, one row per processor.
void draw_processor_heatmap(const DashboardModel& model) {
  const std::vector<std::string>& processors = model.processors();
  const std::optional<WallTime> time = model.metrics_time();
  if (processors.empty() || !time) {
    return;
  }
  const auto count = processors.size();
  const double now = tray_model::chart_time(*time);
  const double start = now - heatmap_seconds;
  std::vector<double> cells(count * static_cast<std::size_t>(heatmap_seconds), 0.0);
  for (std::size_t row = 0; row < count; ++row) {
    const ChartSeries* series = live_chart(model, metric::cpu_utility, processors[row]);
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
    ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, static_cast<double>(count), ImPlotCond_Always);
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
  const std::vector<std::string>& processors = model.processors();
  std::vector<double> frequency(processors.size(), 0.0);
  bool any = false;
  for (std::size_t index = 0; index < processors.size(); ++index) {
    const std::optional<double> current = value(model, metric::cpu_frequency, processors[index]);
    if (current) {
      frequency[index] = *current;
      any = true;
    }
  }
  if (!any) {
    muted_text("Per-processor frequency: " + std::string(tray_model::not_available));
    return;
  }
  const auto count = static_cast<double>(processors.size());
  if (ImPlot::BeginPlot("##frequency", ImVec2(-1.0F, chart_height(6.0F)),
                        ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoLegend)) {
    ImPlot::SetupAxes("logical processor", "% of maximum frequency");
    ImPlot::SetupAxisLimits(ImAxis_X1, -0.5, count - 0.5, ImPlotCond_Always);
    ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, std::max(100.0, *std::ranges::max_element(frequency)),
                            ImPlotCond_Always);
    ImPlot::PlotBars("frequency", frequency.data(), static_cast<int>(frequency.size()), 0.7);
    ImPlot::EndPlot();
  }
}

void draw_cpu_memory(const DashboardModel& model, const ChartView& view, UiState& state) {
  draw_range_selector(view, state);
  ImGui::SeparatorText("CPU");
  if (begin_time_plot("##cpu", chart_height(8.0F), view, "%g %%", 100.0)) {
    plot_line("utilization", view.get(metric::cpu_utility));
    plot_line("DPC", view.get(metric::cpu_dpc));
    plot_line("interrupts", view.get(metric::cpu_interrupt));
    ImPlot::EndPlot();
  }
  muted_text("Utilization per logical processor, last 2 minutes (live)");
  draw_processor_heatmap(model);
  draw_processor_frequency(model);

  ImGui::SeparatorText("Memory");
  if (begin_time_plot("##memory", chart_height(9.0F), view, "%g GB")) {
    plot_line("available", view.get(metric::memory_available), gib);
    plot_line("commit", view.get(metric::memory_commit), gib);
    plot_line("commit limit", view.get(metric::memory_commit_limit), gib);
    plot_line("standby", view.get(metric::memory_standby), gib);
    plot_line("modified", view.get(metric::memory_modified), gib);
    plot_line("compressed", view.get(metric::memory_compressed), gib);
    ImPlot::EndPlot();
  }
  if (begin_time_plot("##faults", chart_height(5.0F), view, "%g/s")) {
    plot_line("hard faults (pages read/s)", view.get(metric::memory_hard_faults));
    ImPlot::EndPlot();
  }

  const std::vector<std::string> adapters = model.metrics().entities(metric::gpu_utilization);
  ImGui::SeparatorText("GPU");
  if (adapters.empty()) {
    muted_text(std::string(tray_model::not_available));
    return;
  }
  if (begin_time_plot("##gpu", chart_height(6.0F), view, "%g %%", 100.0)) {
    for (std::size_t i = 0; i < adapters.size(); ++i) {
      plot_line(std::format("adapter {}", i).c_str(),
                view.get(metric::gpu_utilization, adapters[i]));
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

unsigned status_color(tray_model::DriveStatus status) {
  switch (status) {
    case tray_model::DriveStatus::good:
      return color::good;
    case tray_model::DriveStatus::warning:
      return color::warning;
    case tray_model::DriveStatus::failing:
      return color::bad;
    case tray_model::DriveStatus::unknown:
      break;
  }
  return color::muted;
}

// One labeled value of a health card; `notable` values are highlighted.
struct Fact {
  std::string label;
  std::string value;
  bool notable = false;
};

// The values the drive reports; a counter it does not report is left out.
std::vector<Fact> health_facts(const tray_model::DriveHealthRow& drive) {
  std::vector<Fact> facts;
  if (drive.temperature_c) {
    const auto& limit = drive.temperature_limit_c;
    facts.push_back(
        Fact{.label = "Temperature",
             .value = std::format("{} °C", *drive.temperature_c) +
                      (limit ? std::format(" (limit {} °C)", *limit) : " (no rated limit known)"),
             .notable = limit && *drive.temperature_c > *limit});
  }
  if (drive.percent_used) {
    facts.push_back(Fact{.label = "Wear (rated life used)",
                         .value = std::format("{} %", *drive.percent_used),
                         .notable = *drive.percent_used >= 100});
  }
  if (drive.available_spare) {
    facts.push_back(Fact{.label = "Available spare",
                         .value = std::format("{} %", *drive.available_spare),
                         .notable = false});
  }
  const auto counter = [&facts](const char* label, const std::optional<std::int64_t>& count) {
    if (count) {
      facts.push_back(Fact{.label = label, .value = std::to_string(*count), .notable = *count > 0});
    }
  };
  counter("Media errors", drive.media_errors);
  counter("Reallocated sectors", drive.reallocated_sectors);
  counter("Pending sectors", drive.pending_sectors);
  if (drive.power_on_hours) {
    facts.push_back(Fact{.label = "Power-on hours",
                         .value = std::format("{} h", *drive.power_on_hours),
                         .notable = false});
  }
  if (drive.data_written_bytes) {
    facts.push_back(
        Fact{.label = "Data written",
             .value = tray_model::format_bytes(static_cast<double>(*drive.data_written_bytes)),
             .notable = false});
  }
  std::string endurance = drive.endurance_note;
  if (drive.years_left) {
    endurance = std::format("about {:.0f} years left at the current write rate", *drive.years_left);
    if (drive.tbw_used_percent && drive.tbw_rated) {
      endurance +=
          std::format(" ({:.0f} % of {:.0f} TBW used)", *drive.tbw_used_percent, *drive.tbw_rated);
    }
  }
  if (!endurance.empty()) {
    facts.push_back(Fact{.label = "Endurance", .value = std::move(endurance), .notable = false});
  }
  facts.push_back(Fact{.label = "Read from",
                       .value = humanize(drive.source) + " (" + drive.bus + ")",
                       .notable = false});
  return facts;
}

// The health card of a drive: status with its reasons, then every value it reports.
void draw_health(const tray_model::DriveHealthRow& drive) {
  colored_text(status_color(drive.status),
               "Health: " + std::string(tray_model::to_string(drive.status)));
  for (const std::string& reason : drive.reasons) {
    wrapped_text(status_color(drive.status), "- " + reason);
  }
  if (!drive.readable && drive.unreadable_reason) {
    wrapped_text(color::warning, *drive.unreadable_reason);
  }
  const std::vector<Fact> facts = health_facts(drive);
  if (!ImGui::BeginTable("health", 4, ImGuiTableFlags_SizingStretchProp)) {
    return;
  }
  ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 1.0F);
  ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 2.0F);
  ImGui::TableSetupColumn("label2", ImGuiTableColumnFlags_WidthStretch, 1.0F);
  ImGui::TableSetupColumn("value2", ImGuiTableColumnFlags_WidthStretch, 2.0F);
  for (const Fact& fact : facts) {
    ImGui::TableNextColumn();
    muted_text(fact.label);
    ImGui::TableNextColumn();
    ImGui::PushTextWrapPos(0.0F);
    colored_text(fact.notable ? color::warning : color::text, fact.value);
    ImGui::PopTextWrapPos();
  }
  ImGui::EndTable();
}

void draw_disk_charts(const DashboardModel& model, const std::string& disk, const ChartView& view) {
  muted_text(std::format(
      "read {}  write {}  |  {} IOPS  |  latency {} read, {} write  |  queue {}  |  idle {}",
      tray_model::format_rate(value(model, metric::disk_read_bytes, disk)),
      tray_model::format_rate(value(model, metric::disk_write_bytes, disk)),
      tray_model::format_count(value(model, metric::disk_iops, disk)),
      tray_model::format_milliseconds(value(model, metric::disk_read_latency, disk)),
      tray_model::format_milliseconds(value(model, metric::disk_write_latency, disk)),
      tray_model::format_count(value(model, metric::disk_queue, disk)),
      tray_model::format_percent(value(model, metric::disk_idle, disk))));
  if (!ImGui::BeginTable("charts", 4, ImGuiTableFlags_SizingStretchSame)) {
    return;
  }
  const float height = chart_height(6.0F);
  ImGui::TableNextColumn();
  if (begin_time_plot("Throughput##t", height, view, "%g MB/s")) {
    plot_line("read", view.get(metric::disk_read_bytes, disk), mib);
    plot_line("write", view.get(metric::disk_write_bytes, disk), mib);
    ImPlot::EndPlot();
  }
  ImGui::TableNextColumn();
  if (begin_time_plot("IOPS##i", height, view, "%g")) {
    plot_line("transfers/s", view.get(metric::disk_iops, disk));
    ImPlot::EndPlot();
  }
  ImGui::TableNextColumn();
  if (begin_time_plot("Latency##l", height, view, "%g ms")) {
    plot_line("read", view.get(metric::disk_read_latency, disk));
    plot_line("write", view.get(metric::disk_write_latency, disk));
    ImPlot::EndPlot();
  }
  ImGui::TableNextColumn();
  if (begin_time_plot("Queue and idle##q", height, view, "%g %%", 100.0)) {
    ImPlot::SetupAxis(ImAxis_Y2, nullptr, ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
    plot_line("idle", view.get(metric::disk_idle, disk));
    ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
    plot_line("queue", view.get(metric::disk_queue, disk));
    ImPlot::EndPlot();
  }
  ImGui::EndTable();
}

void draw_disk(const DashboardModel& model, const std::string& disk, const ChartView& view) {
  const tray_model::DriveHealthRow* drive = model.drive(disk);
  std::vector<std::string> volumes = model.volumes_on(disk);
  if (volumes.empty() && drive != nullptr) {
    volumes = drive->volumes;
  }
  ImGui::PushID(disk.c_str());
  ImGui::BeginChild("disk", ImVec2(0.0F, 0.0F),
                    ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2F);
  text("Disk " + disk);
  ImGui::PopFont();
  ImGui::SameLine();
  muted_text((drive != nullptr ? drive->model : std::string(tray_model::not_available)) +
             "  |  volumes " + (volumes.empty() ? std::string("none") : join(volumes, ", ")));
  if (drive != nullptr) {
    draw_health(*drive);
  } else {
    muted_text("Health: " + std::string(tray_model::not_available));
  }
  if (value(model, metric::disk_idle, disk)) {
    draw_disk_charts(model, disk, view);
  }
  ImGui::EndChild();
  ImGui::PopID();
}

void draw_disks(const DashboardModel& model, const ChartView& view, UiState& state) {
  draw_range_selector(view, state);
  std::vector<std::string> disks = model.disks();
  if (model.drives()) {
    for (const tray_model::DriveHealthRow& drive : *model.drives()) {
      if (std::ranges::find(disks, drive.disk) == disks.end()) {
        disks.push_back(drive.disk);
      }
    }
    std::ranges::sort(disks, [](const std::string& left, const std::string& right) {
      return tray_model::natural_less(left, right);
    });
  } else {
    muted_text("Health, temperature, wear and error counters: " +
               std::string(tray_model::not_available) + " (not reported by the service).");
  }
  if (disks.empty()) {
    muted_text("No physical disk has been sampled.");
    return;
  }
  for (const std::string& disk : disks) {
    draw_disk(model, disk, view);
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

std::string signature_text(const tray_model::TrustRow& trust) {
  if (trust.status == "signed") {
    return "signed by " + trust.signer.value_or("an unnamed signer");
  }
  return trust.status == "invalid" ? "invalid signature" : "unsigned";
}

// The trust facts and knowledge-base entry of an image, for a tooltip or the explanation.
void draw_trust_details(const tray_model::TrustRow& trust) {
  if (trust.description) {
    wrapped_text(color::text, *trust.description);
    muted_text("Category: " + trust.category.value_or(std::string(tray_model::not_available)));
  } else {
    muted_text("Not in the knowledge base.");
  }
  text("Signature: " + signature_text(trust));
  wrapped_text(color::muted, "Path: " + trust.path);
  muted_text(trust.user_writable ? "Folder: user-writable" : "Folder: protected");
  if (trust.unrecognized) {
    wrapped_text(color::warning, "Unrecognized: " + join(trust.reasons, ", "));
  }
}

// The trust badge of a tree's root: the signature state, "unrecognized" highlighted, and the
// details on hover.
void draw_trust(const std::optional<tray_model::TrustRow>& trust) {
  if (!trust) {
    muted_text(tray_model::not_available);
    return;
  }
  ImGui::BeginGroup();
  unsigned status = color::muted;
  if (trust->status == "signed") {
    status = color::good;
  } else if (trust->status == "invalid") {
    status = color::bad;
  }
  colored_text(status, trust->status);
  if (trust->unrecognized) {
    ImGui::SameLine();
    colored_text(color::warning, "unrecognized");
  }
  ImGui::EndGroup();
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0F);
    draw_trust_details(*trust);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

void explain_button(std::uint32_t pid, UiState& state) {
  if (ImGui::SmallButton("Explain")) {
    state.explained = pid;
    state.requests.emplace_back(tray_model::ExplainProcess{.pid = pid});
  }
}

void draw_process_columns(const tray_model::ProcessRow& row) {
  ImGui::TableNextColumn();
  text(std::to_string(row.pid));
  ImGui::TableNextColumn();
  text(tray_model::format_percent(row.cpu_percent));
  ImGui::TableNextColumn();
  text(tray_model::format_bytes(static_cast<double>(row.private_working_set.count())));
  ImGui::TableNextColumn();
  text(tray_model::format_bytes(static_cast<double>(row.private_bytes.count())));
}

void draw_tree(const tray_model::ProcessTreeRow& tree, UiState& state) {
  const tray_model::ProcessRow& root = tree.members.front();
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::PushID(static_cast<int>(root.pid));
  const bool single = tree.members.size() == 1;
  const std::string label =
      single ? root.image : std::format("{}  ({} processes)", root.image, tree.members.size());
  // AllowOverlap: the Explain button on the row stays clickable.
  constexpr ImGuiTreeNodeFlags row_flags =
      ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_AllowOverlap;
  const bool open = ImGui::TreeNodeEx(
      label.c_str(),
      row_flags | (single ? ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen
                          : ImGuiTreeNodeFlags_None));
  draw_process_columns(tray_model::ProcessRow{.pid = root.pid,
                                              .image = root.image,
                                              .cpu_percent = tree.cpu_percent,
                                              .private_working_set = tree.private_working_set,
                                              .private_bytes = tree.private_bytes});
  ImGui::TableNextColumn();
  draw_trust(tree.trust);
  ImGui::TableNextColumn();
  explain_button(root.pid, state);
  if (open && !single) {
    for (const tray_model::ProcessRow& member : tree.members) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::PushID(static_cast<int>(member.pid));
      ImGui::TreeNodeEx(member.image.c_str(),
                        row_flags | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
      draw_process_columns(member);
      ImGui::TableNextColumn();
      ImGui::TableNextColumn();
      explain_button(member.pid, state);
      ImGui::PopID();
    }
    ImGui::TreePop();
  }
  ImGui::PopID();
}

void draw_process_table(const std::vector<tray_model::ProcessTreeRow>& trees, UiState& state) {
  if (!ImGui::BeginTable("trees", 7,
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
  ImGui::TableSetupColumn("Trust", ImGuiTableColumnFlags_WidthStretch, 1.6F);
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.8F);
  ImGui::TableHeadersRow();
  for (const tray_model::ProcessTreeRow& tree : trees) {
    draw_tree(tree, state);
  }
  ImGui::EndTable();
}

void draw_explanation_body(const tray_model::Explanation& explanation) {
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2F);
  text(std::format("{} (PID {})", explanation.image, explanation.pid));
  ImGui::PopFont();
  const std::string tree =
      explanation.tree_pid
          ? std::format("In the {} tree (PID {}, {} processes)  |  ", explanation.tree_image,
                        *explanation.tree_pid, explanation.members.size())
          : std::string();
  muted_text(std::format("{}CPU {}  |  memory {}", tree,
                         tray_model::format_percent(explanation.cpu_percent),
                         tray_model::format_percent(explanation.memory_percent)));
  if (explanation.kb) {
    const tray_model::KbRow& kb = *explanation.kb;
    wrapped_text(color::text, kb.description);
    muted_text(std::format("Category: {}  |  needed at startup: {}  |  safe to remove: {}",
                           kb.category, kb.needed_at_startup ? "yes" : "no",
                           kb.safe_to_remove ? "yes" : "no"));
  }
  if (explanation.trust) {
    draw_trust_details(*explanation.trust);
  } else {
    wrapped_text(color::muted, "Path: " + explanation.path.value_or("not readable") +
                                   "  |  trust: n/a (the image could not be read)");
  }
}

// The panel of the process the owner asked to explain, until it is closed.
void draw_explanation(const DashboardModel& model, UiState& state) {
  if (!state.explained) {
    return;
  }
  const std::uint32_t pid = *state.explained;
  ImGui::BeginChild("explanation", ImVec2(0.0F, 0.0F),
                    ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
  if (ImGui::SmallButton("Close")) {
    state.explained.reset();
  }
  const auto& explanation = model.explanation();
  if (!explanation || (explanation->has_value() && (*explanation)->pid != pid)) {
    muted_text(std::format("Explaining PID {}...", pid));
  } else if (!explanation->has_value()) {
    wrapped_text(color::bad, "Explain failed: " + explanation->error().message);
  } else {
    draw_explanation_body(**explanation);
  }
  ImGui::EndChild();
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
  muted_text("CPU is a share of all logical processors. Trust is of the tree's root; hover it.");
  draw_explanation(model, state);
  const auto& trees =
      state.sort == tray_model::ProcessSort::cpu ? model.top_by_cpu() : model.top_by_memory();
  if (!trees) {
    muted_text("Process trees: " + std::string(tray_model::not_available));
    return;
  }
  draw_process_table(*trees, state);
}

unsigned severity_color(tray_model::AlertSeverity severity) {
  switch (severity) {
    case tray_model::AlertSeverity::info:
      return color::accent;
    case tray_model::AlertSeverity::warning:
      return color::warning;
    case tray_model::AlertSeverity::critical:
      break;
  }
  return color::bad;
}

void draw_alert_actions(const tray_model::AlertRow& alert, UiState& state) {
  const auto snooze = [&](const char* label, std::chrono::minutes duration) {
    if (ImGui::SmallButton(label)) {
      state.requests.emplace_back(tray_model::SnoozeAlert{
          .rule = alert.rule, .entity = alert.entity, .duration = duration});
    }
  };
  snooze("Snooze 1 h", std::chrono::hours{1});
  ImGui::SameLine();
  snooze("Snooze 1 day", std::chrono::days{1});
  if (alert.active) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Dismiss")) {
      state.requests.emplace_back(
          tray_model::DismissAlert{.rule = alert.rule, .entity = alert.entity});
    }
  }
}

void draw_alert(const tray_model::AlertRow& alert, const std::chrono::time_zone& zone,
                UiState& state) {
  ImGui::TableNextRow();
  ImGui::PushID((alert.rule + "/" + alert.entity).c_str());
  ImGui::TableNextColumn();
  colored_text(severity_color(alert.severity), tray_model::to_string(alert.severity));
  ImGui::TableNextColumn();
  text(alert.title);
  wrapped_text(color::muted, alert.detail.empty() ? alert.entity : alert.detail);
  ImGui::TableNextColumn();
  muted_text("first " + local_date_time(zone, alert.first));
  muted_text("last " + local_date_time(zone, alert.last));
  if (alert.snoozed_until) {
    colored_text(color::accent, "snoozed until " + local_date_time(zone, *alert.snoozed_until));
  }
  ImGui::TableNextColumn();
  text(std::to_string(alert.count));
  ImGui::TableNextColumn();
  draw_alert_actions(alert, state);
  ImGui::PopID();
}

void draw_alert_table(const char* id, const std::vector<const tray_model::AlertRow*>& alerts,
                      const std::chrono::time_zone& zone, UiState& state) {
  if (!ImGui::BeginTable(id, 5,
                         ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                             ImGuiTableFlags_SizingStretchProp)) {
    return;
  }
  ImGui::TableSetupColumn("Severity", ImGuiTableColumnFlags_WidthStretch, 0.7F);
  ImGui::TableSetupColumn("Alert", ImGuiTableColumnFlags_WidthStretch, 3.0F);
  ImGui::TableSetupColumn("When", ImGuiTableColumnFlags_WidthStretch, 1.6F);
  ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthStretch, 0.5F);
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 2.0F);
  ImGui::TableHeadersRow();
  for (const tray_model::AlertRow* alert : alerts) {
    draw_alert(*alert, zone, state);
  }
  ImGui::EndTable();
}

void draw_alerts(const DashboardModel& model, const std::chrono::time_zone& zone, UiState& state) {
  const auto& badge = model.badge();
  if (!badge) {
    muted_text("Alerts: " + std::string(tray_model::not_available) +
               " (not reported by the service).");
  } else if (badge->active == 0) {
    colored_text(color::good, "No active alert.");
  } else {
    colored_text(badge->worst ? severity_color(*badge->worst) : color::text,
                 std::format("{} active alert{}", badge->active, badge->active == 1 ? "" : "s"));
  }
  if (!model.alert_action_error().empty()) {
    wrapped_text(color::bad, model.alert_action_error());
  }
  const auto& alerts = model.alerts();
  if (!alerts) {
    return;
  }
  std::vector<const tray_model::AlertRow*> active;
  std::vector<const tray_model::AlertRow*> ended;
  for (const tray_model::AlertRow& alert : *alerts) {
    (alert.active ? active : ended).push_back(&alert);
  }
  ImGui::SeparatorText("Active");
  if (active.empty()) {
    muted_text("None.");
  } else {
    draw_alert_table("active", active, zone, state);
  }
  ImGui::SeparatorText("Ended in the last 30 days");
  if (ended.empty()) {
    muted_text("None.");
  } else {
    draw_alert_table("ended", ended, zone, state);
  }
}

void draw_watchdog_status(const DashboardModel& model) {
  if (!model.watchdog_error().empty()) {
    wrapped_text(color::bad, "The freeze watchdog is not running: " + model.watchdog_error());
  } else {
    wrapped_text(color::muted,
                 "The freeze watchdog times Ctrl+C and Ctrl+V only; no other keystroke and no "
                 "clipboard content is recorded. Freezes over 250 ms are listed here.");
  }
  const auto& counts = model.incident_counts();
  if (!counts) {
    return;
  }
  if (counts->queued > 0) {
    colored_text(color::warning,
                 std::format("{} freezes are waiting for the service.", counts->queued));
  }
  if (counts->dropped > 0) {
    colored_text(color::warning, std::format("{} older freezes were dropped while the service "
                                             "was not running.",
                                             counts->dropped));
  }
  if (counts->rejected > 0) {
    colored_text(color::bad,
                 std::format("{} freezes were refused by the service.", counts->rejected));
  }
}

void draw_freeze_group(const tray_model::FreezeGroupRow& group, const std::chrono::time_zone& zone,
                       int index) {
  ImGui::PushID(index);
  const std::string header =
      std::format("{}  -  {}  -  {} freeze{}, latest {}", humanize(group.mechanism),
                  group.process.value_or("no process named"), group.count,
                  group.count == 1 ? "" : "s", local_date_time(zone, group.latest));
  if (ImGui::CollapsingHeader(header.c_str())) {
    for (const tray_model::FreezeRow& incident : group.incidents) {
      const std::string label = std::format(
          "{}  {}  {} ms{}###{}", local_date_time(zone, incident.time), incident.chord,
          incident.latency_ms, incident.cause ? "  (" + *incident.cause + ")" : "", incident.id);
      if (ImGui::TreeNode(label.c_str())) {
        for (const std::string& evidence : incident.evidence) {
          wrapped_text(color::text, "- " + evidence);
        }
        ImGui::TreePop();
      }
    }
    if (group.count > static_cast<std::int64_t>(group.incidents.size())) {
      muted_text(std::format("and {} older",
                             group.count - static_cast<std::int64_t>(group.incidents.size())));
    }
  }
  ImGui::PopID();
}

void draw_freezes(const DashboardModel& model, const std::chrono::time_zone& zone) {
  draw_watchdog_status(model);
  ImGui::Spacing();
  const auto& freezes = model.freezes();
  if (!freezes) {
    muted_text("Freezes: " + std::string(tray_model::not_available));
    return;
  }
  if (freezes->empty()) {
    text("No freeze over 250 ms has been recorded since the watchdog started.");
    return;
  }
  for (std::size_t i = 0; i < freezes->size(); ++i) {
    draw_freeze_group((*freezes)[i], zone, static_cast<int>(i));
  }
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
  set(ImGuiCol_Button, color::raised);
  set(ImGuiCol_ButtonHovered, color::hover);
  set(ImGuiCol_ButtonActive, color::hover);
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

  // A navigation column on the left, the chosen section on the right.
  static constexpr std::array<std::pair<Section, const char*>, 9> sections = {{
      {Section::cpu_memory, "CPU and memory"},
      {Section::disks, "Disks"},
      {Section::volumes, "Volumes"},
      {Section::processes, "Processes"},
      {Section::alerts, "Alerts"},
      {Section::freezes, "Freezes"},
      {Section::cleanup, "Cleanup"},
      {Section::space, "Space"},
      {Section::governor, "Governor"},
  }};
  ImGui::BeginChild("navigation", ImVec2(ImGui::GetFontSize() * 10.0F, 0.0F),
                    ImGuiChildFlags_Borders);
  const int pending = model.service_reachable() ? model.cleanup().pending() : 0;
  const int governor_pending = model.governor_pending();
  for (const auto& [section, label] : sections) {
    // The inbox and the governor show how many decisions wait for the owner; "###" keeps the id
    // stable.
    int waiting = 0;
    if (section == Section::cleanup) {
      waiting = pending;
    } else if (section == Section::governor) {
      waiting = governor_pending;
    }
    const std::string shown =
        waiting > 0 ? std::format("{} ({})###{}", label, waiting, label) : std::string(label);
    if (ImGui::Selectable(shown.c_str(), state.section == section)) {
      state.section = section;
    }
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("section", ImVec2(0.0F, 0.0F), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground);
  const ChartView view = chart_view(model, state.range);
  switch (state.section) {
    case Section::cpu_memory:
      draw_cpu_memory(model, view, state);
      break;
    case Section::disks:
      draw_disks(model, view, state);
      break;
    case Section::volumes:
      draw_volumes(model);
      break;
    case Section::processes:
      draw_processes(model, state);
      break;
    case Section::alerts:
      draw_alerts(model, zone, state);
      break;
    case Section::freezes:
      draw_freezes(model, zone);
      break;
    case Section::cleanup:
      draw_cleanup(model, zone, state);
      break;
    case Section::space:
      draw_space(model, zone, state);
      break;
    case Section::governor:
      draw_governor(model, zone, state);
      break;
  }
  ImGui::EndChild();
  ImGui::End();
}

}  // namespace myclean::tray
