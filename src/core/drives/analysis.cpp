#include "core/drives/analysis.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/drives/history.hpp"
#include "core/drives/layout.hpp"
#include "core/health/collector.hpp"
#include "core/model/enum_names.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/telemetry/volume_info.hpp"

namespace myclean::drives {

namespace {

constexpr std::array<std::string_view, 3> media_names = {"ssd", "hdd", "unknown"};
constexpr std::array<std::string_view, 3> maintenance_names = {"none", "retrim", "defrag"};

Media media_of(const std::vector<std::uint32_t>& disks, const placement::ISeekPenalty& seek) {
  std::optional<Media> media;
  for (const std::uint32_t disk : disks) {
    const std::optional<bool> penalty = seek.seek_penalty(disk);
    if (!penalty) {
      return Media::unknown;
    }
    const Media this_disk = *penalty ? Media::hdd : Media::ssd;
    if (media && *media != this_disk) {
      return Media::unknown;
    }
    media = this_disk;
  }
  return media.value_or(Media::unknown);
}

std::optional<bool> trim_of(const std::vector<std::uint32_t>& disks, const ITrimSupport& trim) {
  std::optional<bool> all = disks.empty() ? std::nullopt : std::optional{true};
  for (const std::uint32_t disk : disks) {
    const std::optional<bool> supported = trim.trim_supported(disk);
    if (supported == false) {
      return false;
    }
    if (!supported) {
      all.reset();
    }
  }
  return all;
}

std::string date(WallTime time) {
  return std::format("{:%Y-%m-%d}", std::chrono::floor<std::chrono::days>(time));
}

std::int64_t days_between(WallTime earlier, WallTime later) {
  return std::chrono::floor<std::chrono::days>(later - earlier).count();
}

std::string days_ago(std::int64_t days) {
  if (days == 0) {
    return "today";
  }
  return days == 1 ? std::string("1 day ago") : std::format("{} days ago", days);
}

void recommend_ssd(VolumeAnalysis& analysis, WallTime now, const Thresholds& thresholds) {
  if (analysis.drive.trim_supported == false) {
    analysis.reasons.emplace_back("the disk does not accept TRIM, so there is nothing to retrim");
    return;
  }
  if (const std::optional<WallTime> last = analysis.last.retrim) {
    const std::int64_t age = days_between(*last, now);
    if (age > thresholds.retrim_age.count()) {
      analysis.needed = Maintenance::retrim;
      analysis.reasons.push_back(std::format("last retrim {} ({}), older than {} days", date(*last),
                                             days_ago(age), thresholds.retrim_age.count()));
    } else {
      analysis.reasons.push_back(
          std::format("retrimmed {} ({}); nothing to do", date(*last), days_ago(age)));
    }
    return;
  }
  analysis.needed = Maintenance::retrim;
  if (!analysis.history_note.empty()) {
    analysis.reasons.push_back("the optimizer log could not be read (" + analysis.history_note +
                               "), so the last retrim is unknown");
  } else if (analysis.last.log_since) {
    analysis.reasons.push_back("no retrim in the optimizer log, which reaches back to " +
                               date(*analysis.last.log_since));
  } else {
    analysis.reasons.emplace_back("the optimizer log holds no optimization at all");
  }
}

void recommend_hdd(VolumeAnalysis& analysis, const Thresholds& thresholds) {
  if (analysis.last.defrag) {
    analysis.reasons.push_back("last defragmentation " + date(*analysis.last.defrag));
  }
  const std::optional<FragmentationSample>& sample = analysis.fragmentation;
  if (!sample || sample->files < thresholds.min_sample_files) {
    analysis.reasons.push_back(
        std::format("only {} large files were sampled, too few to judge fragmentation",
                    sample ? sample->files : 0));
    return;
  }
  const double percent = sample->fragmented_percent();
  const std::string facts =
      std::format("{:.1f} % of the sampled bytes are in fragmented files ({} of {} files)", percent,
                  sample->fragmented_files, sample->files);
  if (percent > thresholds.defrag_percent) {
    analysis.needed = Maintenance::defrag;
    analysis.reasons.push_back(std::format("{}, above {:.0f} %", facts, thresholds.defrag_percent));
  } else {
    analysis.reasons.push_back(facts + "; nothing to do");
  }
}

char letter_of(const std::filesystem::path& root) {
  const std::wstring& text = root.native();
  return text.empty() ? char{0} : static_cast<char>(std::towupper(text.front()));
}

}  // namespace

std::string_view to_string(Media media) {
  return detail::enum_name(media_names, media);
}

std::string_view to_string(Maintenance maintenance) {
  return detail::enum_name(maintenance_names, maintenance);
}

std::vector<DriveState> drive_states(std::span<const telemetry::VolumeStatus> volumes,
                                     const health::HealthView* health,
                                     const placement::ISeekPenalty& seek_penalty,
                                     const ITrimSupport& trim) {
  std::vector<DriveState> states;
  const std::optional<bool> ntfs_trim = trim.ntfs_trim_enabled();
  for (const telemetry::VolumeStatus& volume : volumes) {
    const std::vector<placement::TargetVolume> targets =
        placement::target_volumes(std::span{&volume, 1}, health, seek_penalty);
    if (targets.empty()) {
      continue;
    }
    const placement::TargetVolume& target = targets.front();
    states.push_back(DriveState{.root = target.root,
                                .disks = volume.disks,
                                .drive_class = target.drive_class,
                                .media = media_of(volume.disks, seek_penalty),
                                .healthy = target.healthy,
                                .health_note = target.health_note,
                                .size = volume.size,
                                .free = volume.free,
                                .trim_supported = trim_of(volume.disks, trim),
                                .ntfs_trim_enabled = ntfs_trim});
  }
  return states;
}

Result<std::vector<DriveState>> LiveDriveStates::drives() const {
  Result<std::vector<telemetry::VolumeStatus>> volumes = volumes_->volumes();
  if (!volumes) {
    return tl::unexpected(volumes.error());
  }
  const std::shared_ptr<const health::HealthView> view = health_();
  return drive_states(*volumes, view.get(), *seek_, *trim_);
}

void recommend(VolumeAnalysis& analysis, WallTime now, const Thresholds& thresholds) {
  analysis.needed = Maintenance::none;
  analysis.recommended = Maintenance::none;
  analysis.reasons.clear();
  switch (analysis.drive.media) {
    case Media::ssd:
      recommend_ssd(analysis, now, thresholds);
      break;
    case Media::hdd:
      recommend_hdd(analysis, thresholds);
      break;
    case Media::unknown:
      analysis.reasons.emplace_back(
          "the medium is unknown (the disk reports no seek penalty), so nothing is done");
      break;
  }
  if (analysis.needed == Maintenance::none) {
    return;
  }
  if (!analysis.drive.healthy) {
    analysis.reasons.push_back(
        std::format("{} would be due, but the drive is not maintained "
                    "while its health is not good: {}",
                    to_string(analysis.needed), analysis.drive.health_note));
    return;
  }
  analysis.recommended = analysis.needed;
}

std::vector<VolumeAnalysis> analyze(std::span<const DriveState> drives, const AnalysisTools& tools,
                                    const AnalysisOptions& options) {
  std::vector<Optimization> history;
  std::string history_note;
  if (Result<std::vector<OptimizerEvent>> events = tools.log->events(); events) {
    for (const OptimizerEvent& event : *events) {
      if (std::optional<Optimization> run = interpret(event)) {
        history.push_back(std::move(*run));
      }
    }
  } else {
    history_note = events.error().message;
  }

  std::vector<VolumeAnalysis> analyses;
  for (const DriveState& drive : drives) {
    VolumeAnalysis analysis{.drive = drive,
                            .last = last_runs(history, letter_of(drive.root)),
                            .history_note = history_note};
    if (drive.media == Media::hdd) {
      analysis.fragmentation = sample_fragmentation(drive.root, *tools.reader, *tools.layout,
                                                    *tools.clock, options.sample);
      if (Result<FreeSpaceLayout> free =
              tools.layout->free_space(drive.root, options.bitmap_budget);
          free) {
        analysis.free_space = *free;
      } else {
        analysis.free_space_note = free.error().message;
      }
    }
    recommend(analysis, tools.clock->now(), options.thresholds);
    analyses.push_back(std::move(analysis));
  }
  return analyses;
}

}  // namespace myclean::drives
