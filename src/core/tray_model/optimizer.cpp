#include "core/tray_model/optimizer.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ipc/plans.hpp"
#include "core/ipc/resident.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/tray_model/cleaning.hpp"
#include "core/tray_model/format.hpp"
#include "core/tray_model/json_read.hpp"

namespace myclean::tray_model {

namespace {

using Json = nlohmann::json;
using json_read::array;
using json_read::bytes;
using json_read::optional_number;
using json_read::optional_text;
using json_read::text;

AnalysisState analysis_of(const Json& reply) {
  return AnalysisState{.state = reply.at("state").get<std::string>(),
                       .scanned_at = json_read::optional_time(reply, "scanned_at"),
                       .error = text(reply, "error")};
}

std::string size_text(Bytes value) {
  return format_bytes(static_cast<double>(value.count()));
}

std::string seconds_text(double seconds) {
  return std::format("{:.1f} s", seconds);
}

// The cache a relocate plan moves, from the title the executor gives it ("Relocate npm to D:\...").
std::string_view relocated_cache(const PlanRow& plan) {
  for (const std::string_view cache : relocatable_caches) {
    if (plan.title.starts_with(std::format("Relocate {} to ", cache))) {
      return cache;
    }
  }
  return {};
}

}  // namespace

Result<std::vector<SystemSettingRow>> parse_system_space(const Json& reply) {
  return json_read::parsed(ipc::system_space_status_method, [&] {
    std::vector<SystemSettingRow> rows;
    for (const Json& setting : reply.at("settings")) {
      const std::optional<std::int64_t> cost = json_read::optional_int(setting, "cost");
      rows.push_back(SystemSettingRow{
          .kind = setting.at("kind").get<std::string>(),
          .current = text(setting, "current"),
          .cost = cost ? std::optional(Bytes{static_cast<std::uint64_t>(*cost)}) : std::nullopt,
          .recommended = optional_text(setting, "recommended"),
          .restart_required = json_read::optional_bool(setting, "restart_required").value_or(false),
          .error = text(setting, "error")});
    }
    return rows;
  });
}

std::string_view setting_name(std::string_view kind) {
  if (kind == "hibernation") {
    return "Hibernation";
  }
  if (kind == "page_file") {
    return "Page file";
  }
  if (kind == "shadow_storage") {
    return "Restore points (shadow storage of the system drive)";
  }
  return kind;
}

std::vector<RelocationRow> relocations(const std::vector<PlanRow>& plans) {
  std::vector<RelocationRow> rows;
  for (const std::string_view cache : relocatable_caches) {
    const auto newest = std::ranges::find_if(plans, [&](const PlanRow& plan) {
      return plan.kind == "relocate" && relocated_cache(plan) == cache;
    });
    rows.push_back(RelocationRow{.cache = std::string(cache),
                                 .plan = newest == plans.end() ? nullptr : &*newest});
  }
  for (const PlanRow& plan : plans) {
    if (plan.kind == "relocate" && relocated_cache(plan).empty()) {
      rows.push_back(RelocationRow{.cache = {}, .plan = &plan});
    }
  }
  return rows;
}

std::vector<const PlanRow*> applied_optimizations(const std::vector<PlanRow>& plans) {
  static constexpr std::array<std::string_view, 4> applied_states = {"applied", "verified",
                                                                     "rolled_back", "failed"};
  std::vector<const PlanRow*> rows;
  for (const PlanRow& plan : plans) {
    if ((plan.kind == "system_setting" || plan.kind == "compact") &&
        std::ranges::find(applied_states, plan.state) != applied_states.end()) {
      rows.push_back(&plan);
    }
  }
  return rows;
}

std::string_view verification_text(std::string_view verification) {
  if (verification == "pending") {
    return "measured again later";
  }
  if (verification == "error") {
    return "could not be measured";
  }
  if (verification.empty()) {
    return "not measured yet";
  }
  return verification;  // better, unchanged, worse
}

Result<IndexedFolders> parse_indexed_folders(const Json& reply) {
  return json_read::parsed(ipc::space_indexed_folders_method, [&] {
    IndexedFolders folders{.analysis = analysis_of(reply), .folders = {}};
    for (const Json& folder : array(reply, "folders")) {
      folders.folders.push_back(IndexedFolderRow{.id = folder.at("id").get<std::int64_t>(),
                                                 .path = folder.at("path").get<std::string>(),
                                                 .category = text(folder, "category"),
                                                 .label = text(folder, "label")});
    }
    return folders;
  });
}

Result<VirtualDisks> parse_vhdx(const Json& reply) {
  return json_read::parsed(ipc::space_vhdx_method, [&] {
    VirtualDisks disks{.analysis = analysis_of(reply),
                       .disks = {},
                       .total = bytes(reply, "total"),
                       .notes = json_read::texts(reply, "notes")};
    for (const Json& disk : array(reply, "disks")) {
      disks.disks.push_back(
          VirtualDiskRow{.id = disk.at("id").get<std::int64_t>(),
                         .path = disk.at("path").get<std::string>(),
                         .label = text(disk, "label"),
                         .distribution = text(disk, "distribution"),
                         .size = bytes(disk, "size"),
                         .size_on_disk = bytes(disk, "size_on_disk"),
                         .in_use = json_read::optional_bool(disk, "in_use").value_or(false)});
    }
    return disks;
  });
}

Result<DevDriveAdvice> parse_dev_drive(const Json& reply) {
  return json_read::parsed(ipc::space_dev_drive_method, [&] {
    DevDriveAdvice advice{
        .analysis = analysis_of(reply),
        .supported = json_read::optional_bool(reply, "supported").value_or(false),
        .build = text(reply, "build"),
        .explanation = text(reply, "explanation"),
        .repositories = static_cast<std::int64_t>(array(reply, "repositories").size()),
        .caches = static_cast<std::int64_t>(array(reply, "caches").size()),
        .total = bytes(reply, "total_bytes"),
        .recommended = bytes(reply, "recommended_bytes"),
        .steps = json_read::texts(reply, "steps"),
        .notes = json_read::texts(reply, "notes")};
    for (const Json& drive : array(reply, "dev_drives")) {
      const bool trusted = json_read::optional_bool(drive, "trusted").value_or(false);
      advice.dev_drives.push_back(std::format("{} ({}{})", text(drive, "volume"),
                                              text(drive, "file_system"),
                                              trusted ? ", trusted" : ", not trusted"));
    }
    if (const Json& host = json_read::member(reply, "host"); host.is_object()) {
      advice.host = std::format("{} ({}, {} free)", text(host, "volume"), text(host, "drive_class"),
                                size_text(bytes(host, "free")));
    }
    return advice;
  });
}

Result<std::vector<ResidentCostRow>> parse_resident_costs(const Json& reply) {
  return json_read::parsed(ipc::resident_costs_method, [&] {
    std::vector<ResidentCostRow> rows;
    for (const Json& program : reply.at("programs")) {
      rows.push_back(
          ResidentCostRow{.image = program.at("image").get<std::string>(),
                          .hours_running = optional_number(program, "hours_running").value_or(0.0),
                          .cpu_seconds = optional_number(program, "cpu_seconds").value_or(0.0),
                          .ram_average = bytes(program, "ram_average"),
                          .ram_peak = bytes(program, "ram_peak"),
                          .disk = bytes(program, "disk_bytes"),
                          .network = bytes(program, "net_bytes")});
    }
    return rows;
  });
}

Result<std::vector<StartupItemRow>> parse_startup_list(const Json& reply) {
  return json_read::parsed(ipc::startup_list_method, [&] {
    std::vector<StartupItemRow> rows;
    for (const Json& item : reply.at("items")) {
      std::optional<StartupImpact> impact;
      if (const Json& measured = json_read::member(item, "impact"); measured.is_object()) {
        impact = StartupImpact{
            .source = text(measured, "source"),
            .cpu_seconds = optional_number(measured, "cpu_seconds").value_or(0.0),
            .io_bytes = bytes(measured, "io_bytes"),
            .logons = json_read::optional_int(measured, "logons").value_or(0),
            .delay_seconds = optional_number(measured, "delay_seconds").value_or(0.0)};
      }
      rows.push_back(StartupItemRow{.id = item.at("id").get<std::int64_t>(),
                                    .key = item.at("key").get<std::string>(),
                                    .source = item.at("source").get<std::string>(),
                                    .scope = text(item, "scope"),
                                    .name = text(item, "name"),
                                    .display_name = text(item, "display_name"),
                                    .command = text(item, "command"),
                                    .enabled = json_read::optional_bool(item, "enabled"),
                                    .signer = text(item, "signer"),
                                    .impact = std::move(impact)});
    }
    return rows;
  });
}

std::string_view startup_source_text(std::string_view source) {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 7> names = {{
      {"run", "Run key"},
      {"run32", "Run key (32-bit)"},
      {"runonce", "RunOnce"},
      {"runonce32", "RunOnce (32-bit)"},
      {"folder", "Startup folder"},
      {"task", "Scheduled task"},
      {"service", "Service"},
  }};
  const auto found =
      std::ranges::find(names, source, &std::pair<std::string_view, std::string_view>::first);
  return found == names.end() ? source : found->second;
}

std::string impact_text(const std::optional<StartupImpact>& impact) {
  if (!impact) {
    return "not measured yet";
  }
  if (impact->source == "diagnostics") {
    return std::format("delays the boot by {} (Windows)", seconds_text(impact->delay_seconds));
  }
  return std::format("{} CPU, {} of disk in the first 2 minutes (median of {} logon{})",
                     seconds_text(impact->cpu_seconds), size_text(impact->io_bytes), impact->logons,
                     impact->logons == 1 ? "" : "s");
}

Result<std::vector<std::string>> startup_choice(const std::vector<StartupItemRow>& items,
                                                const std::set<std::string>& ticked, bool enable) {
  std::vector<std::string> keys;
  for (const StartupItemRow& item : items) {
    if (ticked.contains(item.key) && item.enabled && *item.enabled != enable) {
      keys.push_back(item.key);
    }
  }
  if (keys.empty()) {
    return fail(Errc::invalid_argument,
                enable ? "no disabled item is ticked" : "no enabled item is ticked");
  }
  return keys;
}

Result<BootTimes> parse_boot_times(const Json& reply) {
  return json_read::parsed(ipc::startup_boot_times_method, [&] {
    BootTimes times{.log_error = text(reply, "log_error")};
    if (const Json& latest = json_read::member(reply, "latest"); latest.is_object()) {
      times.latest_start = json_read::optional_time(latest, "start");
      times.boot_seconds = latest.at("boot_seconds").get<double>();
      times.logon_seconds = latest.at("logon_seconds").get<double>();
      times.boot_difference = optional_number(latest, "boot_difference_seconds");
      times.logon_difference = optional_number(latest, "logon_difference_seconds");
      for (const Json& contributor : array(latest, "contributors")) {
        const Json& item = json_read::member(contributor, "item");
        times.contributors.push_back(BootContributorRow{
            .image = contributor.at("image").get<std::string>(),
            .cpu_seconds = optional_number(contributor, "cpu_seconds").value_or(0.0),
            .io_bytes = bytes(contributor, "io_bytes"),
            .item = item.is_object() ? text(item, "name") : std::string()});
      }
    }
    if (const Json& median = json_read::member(reply, "median"); median.is_object()) {
      times.median_boot = optional_number(median, "boot_seconds");
      times.median_logon = optional_number(median, "logon_seconds");
      times.median_boots = json_read::optional_int(median, "boots").value_or(0);
    }
    for (const Json& change : array(reply, "changes")) {
      StartupChangeRow row{.plan = change.at("plan").get<std::int64_t>(),
                           .applied = from_unix_seconds(change.at("applied").get<std::int64_t>())};
      for (const Json& item : array(change, "items")) {
        row.items.push_back(text(item, "name"));
      }
      const Json& before = json_read::member(change, "before");
      const Json& after = json_read::member(change, "after");
      row.logon_before =
          before.is_object() ? optional_number(before, "logon_seconds") : std::nullopt;
      row.logon_after = after.is_object() ? optional_number(after, "logon_seconds") : std::nullopt;
      times.changes.push_back(std::move(row));
    }
    return times;
  });
}

}  // namespace myclean::tray_model
