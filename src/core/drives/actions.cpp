#include "core/drives/actions.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/drives/analysis.hpp"
#include "core/model/action.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/settings.hpp"
#include "core/store/time_series.hpp"
#include "core/telemetry/history.hpp"
#include "core/telemetry/metrics.hpp"

namespace myclean::drives {

namespace {

using Json = nlohmann::json;

constexpr double unknown_metric = -1.0;

std::string root_text(const std::filesystem::path& root) {
  const std::u8string text = root.u8string();
  return {text.begin(), text.end()};
}

// "K:" for "K:\".
std::string drive_text(const std::filesystem::path& root) {
  std::string text = root_text(root);
  while (!text.empty() && text.back() == '\\') {
    text.pop_back();
  }
  return text;
}

Effect lower_is_better(double before, double after) {
  if (after < 0.0) {
    return Effect::unchanged;
  }
  if (before < 0.0 || after < before) {
    return Effect::better;
  }
  return after > before ? Effect::worse : Effect::unchanged;
}

}  // namespace

Result<std::optional<double>> StoreDiskIdle::idle_percent(std::uint32_t disk_number,
                                                          std::chrono::seconds window) const {
  const WallTime now = clock_->now();
  Result<telemetry::History> history =
      telemetry::query_history(*store_,
                               SeriesKey{.metric = std::string(telemetry::metric::disk_idle),
                                         .entity = std::to_string(disk_number)},
                               now - window, now, now);
  if (!history) {
    return tl::unexpected(history.error());
  }
  double sum = 0.0;
  std::int64_t samples = 0;
  for (const store::Aggregate& point : history->points) {
    sum += point.mean * static_cast<double>(point.count);
    samples += point.count;
  }
  if (samples == 0 || std::chrono::seconds{samples * 2} < window) {
    return std::optional<double>{};
  }
  return std::optional{sum / static_cast<double>(samples)};
}

std::string unsuitable(Maintenance kind, const DriveState& drive) {
  const std::string root = root_text(drive.root);
  if (kind == Maintenance::retrim) {
    if (drive.media != Media::ssd) {
      return std::format("{} is not on an SSD (medium {}); only an SSD is retrimmed", root,
                         to_string(drive.media));
    }
    if (drive.trim_supported == false) {
      return root + " is on a disk that does not accept TRIM";
    }
    return {};
  }
  if (kind == Maintenance::defrag) {
    return drive.media == Media::hdd
               ? std::string{}
               : std::format("{} is not on an HDD (medium {}); an SSD is never defragmented", root,
                             to_string(drive.media));
  }
  return "no maintenance was named";
}

Result<std::string> blocked(const DriveState& drive, const IDiskIdle& idle,
                            const IPowerSource& power, const RunPolicy& policy) {
  if (!drive.healthy) {
    return "the drive's health is not good: " + drive.health_note;
  }
  Result<bool> battery = power.on_battery();
  if (!battery) {
    return tl::unexpected(battery.error());
  }
  if (*battery) {
    return std::string("the computer runs on battery");
  }
  for (const std::uint32_t disk : drive.disks) {
    Result<std::optional<double>> percent = idle.idle_percent(disk, policy.idle_window);
    if (!percent) {
      return tl::unexpected(percent.error());
    }
    if (!*percent) {
      return std::format("disk {} has too little idle-time history over the last {} min", disk,
                         policy.idle_window.count());
    }
    if (**percent < policy.min_idle_percent) {
      return std::format("disk {} was {:.1f} % idle over the last {} min, below {:.0f} %", disk,
                         **percent, policy.idle_window.count(), policy.min_idle_percent);
    }
  }
  return std::string{};
}

placement::Command maintenance_command(Maintenance kind, const std::filesystem::path& root) {
  return placement::Command{
      .tool = "defrag.exe",
      .arguments = {drive_text(root), kind == Maintenance::retrim ? "/L" : "/D"}};
}

ActionInfo VolumeMaintenance::describe() const {
  const std::string command = maintenance_command(kind_, root_).text();
  if (kind_ == Maintenance::retrim) {
    return {.what = "Retrim " + root_text(root_),
            .why = "tells the SSD which blocks are free so it keeps its write speed; runs \"" +
                   command + "\" at low priority while the disk is idle",
            .risk = Risk::low,
            .tier = Tier::automatic,
            .context = ExecutionContext::service};
  }
  return {.what = "Defragment " + root_text(root_),
          .why = "joins fragmented files on a rotating disk so they read faster; runs \"" +
                 command + "\" at low priority while the disk is idle, which can take hours",
          .risk = Risk::low,
          .tier = Tier::ask,
          .context = ExecutionContext::service};
}

Result<VolumeAnalysis> VolumeMaintenance::current() const {
  Result<std::vector<DriveState>> drives = tools_.drives->drives();
  if (!drives) {
    return tl::unexpected(drives.error());
  }
  for (const DriveState& drive : *drives) {
    if (placement::same_path(drive.root, root_)) {
      std::vector<VolumeAnalysis> analyses =
          analyze(std::span{&drive, 1}, tools_.analysis, options_);
      return std::move(analyses.front());
    }
  }
  return fail(Errc::not_found, "no volume " + root_text(root_));
}

double VolumeMaintenance::metric(const VolumeAnalysis& analysis) const {
  if (kind_ == Maintenance::retrim) {
    if (!analysis.last.retrim) {
      return unknown_metric;
    }
    return static_cast<double>(
        std::chrono::floor<std::chrono::days>(tools_.analysis.clock->now() - *analysis.last.retrim)
            .count());
  }
  return analysis.fragmentation ? analysis.fragmentation->fragmented_percent() : unknown_metric;
}

Result<Preview> VolumeMaintenance::preview() {
  Result<VolumeAnalysis> analysis = current();
  if (!analysis) {
    return tl::unexpected(analysis.error());
  }
  if (const std::string reason = unsuitable(kind_, analysis->drive); !reason.empty()) {
    return fail(Errc::refused, reason);
  }
  analysis_ = std::move(*analysis);
  return Preview{.items = {PlanItem{.path = root_}}};
}

Result<Undo> VolumeMaintenance::apply(const Preview& preview) {
  if (preview.items.size() != 1 || !placement::same_path(preview.items.front().path, root_)) {
    return fail(Errc::invalid_argument, "the preview must hold exactly the volume root");
  }
  Result<VolumeAnalysis> analysis = current();
  if (!analysis) {
    return tl::unexpected(analysis.error());
  }
  if (const std::string reason = unsuitable(kind_, analysis->drive); !reason.empty()) {
    return fail(Errc::refused, reason);
  }
  Result<std::string> reason = blocked(analysis->drive, *tools_.idle, *tools_.power, policy_);
  if (!reason) {
    return tl::unexpected(reason.error());
  }
  if (!reason->empty()) {
    return fail(Errc::refused, "not now: " + *reason);
  }
  const double before = metric(*analysis);
  const placement::Command command = maintenance_command(kind_, root_);
  Result<placement::CommandResult> ran = tools_.runner->run(command);
  if (!ran) {
    return fail(ran.error().code, command.text() + ": " + ran.error().message);
  }
  if (ran->exit_code != 0) {
    return fail(Errc::io_error,
                std::format("{} exited with {}: {}", command.text(), ran->exit_code, ran->output));
  }
  return Undo{.items = {UndoItem{.item = preview.items.front().id,
                                 .data = Json{{"before", before}}.dump()}},
              .not_reversible = std::string(to_string(kind_)) + " needs and has no undo"};
}

Result<Verification> VolumeMaintenance::verify(const Undo& undo) {
  double before = unknown_metric;
  try {
    if (undo.items.size() != 1) {
      return fail(Errc::invalid_data, "maintenance undo data must hold exactly one item");
    }
    before = Json::parse(undo.items.front().data).at("before").get<double>();
  } catch (const Json::exception& error) {
    return fail(Errc::invalid_data,
                std::string("maintenance undo data is malformed: ") + error.what());
  }
  Result<VolumeAnalysis> analysis = current();
  if (!analysis) {
    return tl::unexpected(analysis.error());
  }
  const double after = metric(*analysis);
  return Verification{
      .metric = kind_ == Maintenance::retrim ? "days_since_retrim" : "fragmented_percent",
      .before = before,
      .after = after,
      .effect = lower_is_better(before, after)};
}

Result<void> VolumeMaintenance::undo(const Undo& /*undo*/) {
  return fail(Errc::not_reversible,
              std::string(to_string(kind_)) + " of " + root_text(root_) + " cannot be undone");
}

}  // namespace myclean::drives
