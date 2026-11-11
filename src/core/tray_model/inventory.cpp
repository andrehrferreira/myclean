#include "core/tray_model/inventory.hpp"

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ipc/apps.hpp"
#include "core/ipc/backup.hpp"
#include "core/ipc/security.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/tray_model/json_read.hpp"

namespace myclean::tray_model {

namespace {

using Json = nlohmann::json;
using json_read::array;
using json_read::bytes;
using json_read::member;
using json_read::optional_bool;
using json_read::optional_int;
using json_read::optional_time;
using json_read::text;
using json_read::texts;

AppRow parse_app(const Json& app) {
  const Json& last = member(app, "last_used");
  const std::optional<std::int64_t> size = optional_int(app, "size");
  return AppRow{
      .id = app.at("id").get<std::int64_t>(),
      .kind = text(app, "kind"),
      .name = app.at("name").get<std::string>(),
      .publisher = text(app, "publisher"),
      .version = text(app, "version"),
      .install_date = text(app, "install_date"),
      .size = size ? std::optional(Bytes{static_cast<std::uint64_t>(*size)}) : std::nullopt,
      .description = text(app, "description"),
      .last_used = last.is_object() ? optional_time(last, "time") : std::nullopt,
      .days_ago = last.is_object() ? optional_int(last, "days_ago") : std::nullopt,
      .last_used_source = last.is_object() ? text(last, "source") : std::string(),
      .confidence = last.is_object() ? text(last, "confidence") : std::string(),
      .last_used_text = last.is_object() ? text(last, "text") : std::string(),
      .verdict = text(app, "verdict"),
      .reason = text(app, "reason"),
      .uninstaller = optional_bool(app, "uninstaller").value_or(false)};
}

std::string key_of(std::string_view kind, std::string_view name, std::string_view version,
                   std::string_view publisher) {
  return std::format("{}|{}|{}|{}", kind, name, version, publisher);
}

PersistenceRow parse_persistence(const Json& item) {
  PersistenceRow row{.id = item.at("id").get<std::int64_t>(),
                     .source = item.at("source").get<std::string>(),
                     .location = text(item, "location"),
                     .name = text(item, "name"),
                     .command = text(item, "command"),
                     .image = text(item, "image"),
                     .enabled = optional_bool(item, "enabled"),
                     .signer = text(item, "signer"),
                     .startup_item = text(item, "startup_item"),
                     .status = item.at("status").get<std::string>(),
                     .reasons = texts(item, "reasons"),
                     .summary = text(item, "summary")};
  if (const Json& verdict = member(item, "verdict"); verdict.is_object()) {
    row.verdict_source = text(verdict, "source");
    row.threat = text(verdict, "threat");
  }
  if (const Json& lookup = member(item, "virustotal"); lookup.is_object()) {
    const std::int64_t engines = optional_int(lookup, "engines").value_or(0);
    row.virustotal = engines == 0
                         ? std::string("VirusTotal does not know this file")
                         : std::format("VirusTotal: {} of {} engines flag it",
                                       optional_int(lookup, "flagged").value_or(0), engines);
  }
  return row;
}

TalkerRow parse_talker(const Json& process) {
  TalkerRow row{.pid = process.at("pid").get<std::uint32_t>(),
                .image = text(process, "image"),
                .running = optional_bool(process, "running").value_or(false),
                .visible_window = optional_bool(process, "visible_window"),
                .sent = bytes(process, "sent"),
                .received = bytes(process, "received"),
                .active_minutes = optional_int(process, "active_minutes").value_or(0)};
  if (const Json& trust = member(process, "trust"); trust.is_object()) {
    row.signer = text(trust, "signer");
    row.unrecognized = optional_bool(trust, "unrecognized").value_or(false);
    row.reasons = texts(trust, "reasons");
  }
  for (const Json& remote : array(process, "remotes")) {
    row.remotes.push_back(RemoteRow{.address = text(remote, "address"),
                                    .name = text(remote, "name"),
                                    .port = optional_int(remote, "port").value_or(0),
                                    .protocol = text(remote, "protocol"),
                                    .open_now = optional_bool(remote, "open_now").value_or(false)});
  }
  for (const Json& endpoint : array(process, "listening")) {
    row.listening.push_back(std::format("{} {}", text(endpoint, "protocol"),
                                        optional_int(endpoint, "port").value_or(0)));
  }
  return row;
}

BackupSourceRow parse_source(const Json& source) {
  BackupSourceRow row{
      .source = source.at("source").get<std::string>(),
      .name = text(source, "name"),
      .status = source.at("status").get<std::string>(),
      .location = text(source, "location"),
      .newest = optional_time(source, "newest"),
      .age_days = optional_int(source, "newest_age_days"),
      .copies = optional_int(source, "copies").value_or(0),
      .counts_toward_age = optional_bool(source, "counts_toward_age").value_or(false),
      .detail = text(source, "detail"),
      .extra = texts(source, "products")};
  for (std::string& folder : texts(source, "folders")) {
    row.extra.push_back(std::move(folder));
  }
  for (const Json& artifact : array(source, "artifacts")) {
    row.extra.push_back(text(artifact, "path"));
  }
  return row;
}

void parse_backup_report(const Json& reply, BackupCheck& check) {
  for (const Json& source : array(reply, "sources")) {
    check.sources.push_back(parse_source(source));
  }
  if (const Json& last = member(reply, "last_backup"); last.is_object()) {
    check.has_last_backup = true;
    check.last_name = text(last, "name");
    check.last_location = text(last, "location");
    check.last_time = optional_time(last, "newest");
    check.last_age_days = optional_int(last, "age_days").value_or(0);
  }
  check.backup_exists = optional_bool(reply, "backup_exists").value_or(false);
  check.summary = text(reply, "summary");
  if (const Json& recommendation = member(reply, "recommendation"); recommendation.is_object()) {
    check.recommendation = text(recommendation, "text");
    check.recommended_drives = texts(recommendation, "drives");
    for (const Json& left : array(recommendation, "left_out")) {
      check.left_out.push_back(std::format("{} ({})", text(left, "drive"), text(left, "reason")));
    }
  }
  if (const Json& repositories = member(reply, "repositories"); repositories.is_object()) {
    check.repositories_checked = true;
    check.workspace_roots = texts(repositories, "roots");
    check.repositories = optional_int(repositories, "checked").value_or(0);
    check.repository_problems = texts(repositories, "problems");
    for (const Json& repository : array(repositories, "unprotected")) {
      check.unprotected.push_back(
          UnprotectedRepoRow{.path = text(repository, "path"), .text = text(repository, "text")});
    }
  }
}

}  // namespace

Result<AppsList> parse_apps_list(const Json& reply) {
  return json_read::parsed(ipc::apps_list_method, [&] {
    AppsList list{.count = optional_int(reply, "count").value_or(0),
                  .manufacturer = text(reply, "manufacturer"),
                  .problems = texts(reply, "problems")};
    if (const Json& history = member(reply, "history"); history.is_object()) {
      list.history_days = optional_int(history, "days").value_or(0);
    }
    for (const Json& app : reply.at("apps")) {
      list.apps.push_back(parse_app(app));
    }
    return list;
  });
}

std::string app_key(const AppRow& app) {
  return key_of(app.kind, app.name, app.version, app.publisher);
}

std::string app_key(const Json& app) {
  return key_of(text(app, "kind"), text(app, "name"), text(app, "version"), text(app, "publisher"));
}

bool unused_for(const AppRow& app, std::int64_t days) {
  return !app.days_ago || *app.days_ago >= days;
}

Result<SecurityScan> parse_security_scan(const Json& reply) {
  return json_read::parsed(ipc::security_scan_method, [&] {
    SecurityScan scan{.problems = texts(reply, "problems")};
    for (const Json& item : reply.at("items")) {
      scan.items.push_back(parse_persistence(item));
    }
    if (const Json& counts = member(reply, "counts"); counts.is_object()) {
      scan.defaults = optional_int(counts, "default").value_or(0);
      scan.ok = optional_int(counts, "ok").value_or(0);
      scan.suspicious = optional_int(counts, "suspicious").value_or(0);
      scan.known_dangerous = optional_int(counts, "known_dangerous").value_or(0);
    }
    if (const Json& defender = member(reply, "defender"); defender.is_object()) {
      scan.defender_available = optional_bool(defender, "available").value_or(false);
      scan.defender_error = text(defender, "error");
      for (const Json& detection : array(defender, "detections")) {
        scan.detections.push_back(DefenderDetectionRow{.threat = text(detection, "threat"),
                                                       .resources = texts(detection, "resources"),
                                                       .time = optional_time(detection, "time")});
      }
    }
    if (const Json& virustotal = member(reply, "virustotal"); virustotal.is_object()) {
      scan.virustotal_enabled = optional_bool(virustotal, "enabled").value_or(false);
      scan.virustotal_note = text(virustotal, "note");
      scan.virustotal_queued = optional_int(virustotal, "queued").value_or(0);
    }
    return scan;
  });
}

std::string persistence_key(const PersistenceRow& item) {
  return std::format("{}|{}|{}", item.source, item.location, item.name);
}

std::string persistence_key(const Json& item) {
  return std::format("{}|{}|{}", text(item, "source"), text(item, "location"), text(item, "name"));
}

bool known_dangerous(const PersistenceRow& item) {
  return item.status == "known_dangerous" && !item.verdict_source.empty();
}

std::string trust_label(const PersistenceRow& item) {
  if (known_dangerous(item)) {
    return item.threat.empty()
               ? std::format("known dangerous ({})", item.verdict_source)
               : std::format("known dangerous ({}: {})", item.verdict_source, item.threat);
  }
  if (item.status == "known_dangerous" || item.status == "suspicious") {
    return "suspicious";
  }
  return item.status == "default" ? "Windows default" : "ok";
}

Result<Extensions> parse_security_extensions(const Json& reply) {
  return json_read::parsed(ipc::security_extensions_method, [&] {
    Extensions extensions{.problems = texts(reply, "problems")};
    for (const Json& extension : reply.at("extensions")) {
      extensions.extensions.push_back(
          ExtensionRow{.browser = text(extension, "browser"),
                       .profile = text(extension, "profile"),
                       .id = text(extension, "id"),
                       .name = text(extension, "name"),
                       .version = text(extension, "version"),
                       .origin = text(extension, "origin"),
                       .enabled = optional_bool(extension, "enabled").value_or(false),
                       .flag = text(extension, "flag")});
    }
    return extensions;
  });
}

Result<NetworkTalkers> parse_security_network(const Json& reply) {
  return json_read::parsed(ipc::security_network_method, [&] {
    NetworkTalkers talkers{
        .hours = json_read::optional_number(reply, "hours").value_or(0.0),
        .reverse_dns = optional_bool(reply, "reverse_dns").value_or(false),
        .windows_reported = optional_bool(reply, "windows_reported").value_or(false)};
    for (const Json& process : reply.at("processes")) {
      talkers.processes.push_back(parse_talker(process));
    }
    return talkers;
  });
}

std::string_view window_text(const std::optional<bool>& visible) {
  if (!visible) {
    return "unknown";
  }
  return *visible ? "a visible window" : "no window";
}

Result<BackupCheck> parse_backups_check(const Json& reply) {
  return json_read::parsed(ipc::backups_check_method, [&] {
    BackupCheck check{.state = reply.at("state").get<std::string>(),
                      .checked_at = optional_time(reply, "checked_at"),
                      .error = text(reply, "error")};
    parse_backup_report(reply, check);
    return check;
  });
}

std::string_view backup_status_text(std::string_view status) {
  if (status == "not_found") {
    return "not found";
  }
  if (status == "could_not_check") {
    return "could not be checked";
  }
  return status;
}

}  // namespace myclean::tray_model
