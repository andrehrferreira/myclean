#include "core/scanall/drive_health.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/drives/analysis.hpp"
#include "core/health/analysis.hpp"
#include "core/health/collector.hpp"
#include "core/health/reading.hpp"
#include "core/ipc/disk_health.hpp"
#include "core/model/result.hpp"
#include "core/placement/drives.hpp"
#include "core/scanall/findings.hpp"

namespace myclean::scanall {

namespace {

using Json = nlohmann::json;

std::optional<health::Status> status_of(const std::string& text) {
  if (text == ipc::drive_good) {
    return health::Status::good;
  }
  if (text == ipc::drive_warning) {
    return health::Status::warning;
  }
  if (text == ipc::drive_failing) {
    return health::Status::critical;
  }
  if (text == ipc::drive_unknown) {
    return health::Status::unknown;
  }
  return std::nullopt;
}

Result<health::DriveHealth> drive_of_reply(const Json& drive) {
  const std::string disk = drive.at("disk").get<std::string>();
  const std::string status_text = drive.at("status").get<std::string>();
  const std::optional<health::Status> status = status_of(status_text);
  const std::optional<health::Bus> bus = health::parse_bus(drive.at("bus").get<std::string>());
  std::uint32_t number = 0;
  const char* const last = std::to_address(disk.end());
  if (const auto [end, error] = std::from_chars(disk.data(), last, number);
      error != std::errc{} || end != last) {
    return fail(Errc::invalid_data, "disks.health: not a disk number: " + disk);
  }
  if (!status || !bus) {
    return fail(Errc::invalid_data, "disks.health: unknown status or bus of disk " + disk);
  }
  health::HealthReading reading{
      .model = drive.at("model").get<std::string>(),
      .disk_number = number,
      .bus = *bus,
      .source = health::parse_source(drive.at("source").get<std::string>())
                    .value_or(health::Source::none)};
  if (const Json& reason = drive.at("unreadable_reason"); reason.is_string()) {
    reading.problem = reason.get<std::string>();
  }
  return health::DriveHealth{
      .reading = std::move(reading),
      .assessment = health::Assessment{
          .status = *status, .reasons = drive.at("reasons").get<std::vector<std::string>>()}};
}

int severity(health::Status status) {
  switch (status) {
    case health::Status::good:
      return 0;
    case health::Status::unknown:
      return 1;
    case health::Status::warning:
      return 2;
    case health::Status::critical:
      break;
  }
  return 3;
}

health::Status worst_health(const drives::DriveState& state, const health::HealthView& view) {
  health::Status worst = state.disks.empty() ? health::Status::unknown : health::Status::good;
  for (const std::uint32_t disk : state.disks) {
    const auto found = std::ranges::find(view.drives, disk, [](const health::DriveHealth& drive) {
      return drive.reading.disk_number;
    });
    const health::Status status =
        found == view.drives.end() ? health::Status::unknown : found->assessment.status;
    if (severity(status) > severity(worst)) {
      worst = status;
    }
  }
  return worst;
}

}  // namespace

Result<health::HealthView> parse_service_health(const Json& reply) {
  try {
    health::HealthView view;
    if (const Json& time = reply.at("time"); time.is_number_integer()) {
      view.time = WallTime{std::chrono::seconds{time.get<std::int64_t>()}};
    }
    for (const Json& drive : reply.at("drives")) {
      Result<health::DriveHealth> parsed = drive_of_reply(drive);
      if (!parsed) {
        return tl::unexpected(parsed.error());
      }
      view.drives.push_back(*std::move(parsed));
    }
    return view;
  } catch (const Json::exception& error) {
    return fail(Errc::invalid_data, std::string("disks.health: ") + error.what());
  }
}

ChosenHealth choose_health(const Result<Json>& service,
                           const std::function<health::HealthView()>& local) {
  std::string why;
  if (!service) {
    why = service.error().message;
  } else if (Result<health::HealthView> view = parse_service_health(*service); !view) {
    why = view.error().message;
  } else if (view->drives.empty()) {
    why = "it has not read the disks yet";
  } else {
    const auto read = std::chrono::floor<std::chrono::minutes>(view->time);
    return ChosenHealth{
        .view = *std::move(view),
        .note =
            std::format("disk health: from the MyClean service, read {:%Y-%m-%d %H:%M} UTC", read)};
  }
  ChosenHealth chosen{
      .view = local(),
      .note = "disk health: read by this process, not the MyClean service (" + why + ")"};
  std::string unknown;
  for (const health::DriveHealth& drive : chosen.view.drives) {
    if (drive.assessment.status == health::Status::unknown) {
      unknown += std::format("{}disk {} ({})", unknown.empty() ? "" : ", ",
                             drive.reading.disk_number, drive.reading.model);
    }
  }
  if (!unknown.empty()) {
    chosen.note += "; of unknown health, so never an archive or relocation target: " + unknown;
  }
  return chosen;
}

std::vector<DriveInfo> drive_infos(std::span<const drives::DriveState> states,
                                   const health::HealthView& view,
                                   const std::filesystem::path& system_root) {
  const std::wstring system = placement::drive_root(system_root);
  std::vector<DriveInfo> infos;
  for (const drives::DriveState& state : states) {
    infos.push_back(DriveInfo{.root = state.root,
                              .drive_class = state.drive_class,
                              .health = worst_health(state, view),
                              .health_note = state.health_note,
                              .size = state.size,
                              .free = state.free,
                              .system = placement::drive_root(state.root) == system});
  }
  return infos;
}

}  // namespace myclean::scanall
