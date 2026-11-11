#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/tray_model/screens.hpp"

// Answers of the screens' methods, for the screens' model tests and the dashboard's drawing tests.
namespace myclean::tray_model::testing {

using Json = nlohmann::json;

inline constexpr std::int64_t day_seconds = 86400;
inline constexpr std::int64_t now_seconds = 1'800'000'000;

inline Json system_space_json() {
  return Json{{"settings",
               {{{"kind", "hibernation"},
                 {"current", "full (hiberfil.sys 50.9 GB)"},
                 {"cost", 54'659'000'000},
                 {"recommended", "reduced (about 25.6 GB)"},
                 {"restart_required", false},
                 {"error", nullptr}},
                {{"kind", "page_file"},
                 {"current", "system-managed"},
                 {"cost", 17'179'869'184},
                 {"recommended", nullptr},
                 {"restart_required", true},
                 {"error", nullptr}},
                {{"kind", "shadow_storage"},
                 {"current", nullptr},
                 {"cost", nullptr},
                 {"recommended", nullptr},
                 {"restart_required", false},
                 {"error", "C: has no shadow storage"}}}}};
}

inline Json startup_item_json(std::int64_t id, const std::string& key, Json enabled, Json impact) {
  return Json{{"id", id},
              {"key", key},
              {"source", "run"},
              {"scope", "user"},
              {"name", key},
              {"display_name", key},
              {"command", key + ".exe"},
              {"enabled", std::move(enabled)},
              {"impact", std::move(impact)}};
}

inline Json startup_json() {
  return Json{{"items",
               {startup_item_json(1, "run:user:Discord", true,
                                  Json{{"source", "myclean"},
                                       {"cpu_seconds", 1.25},
                                       {"io_bytes", 36'700'160},
                                       {"logons", 5},
                                       {"delay_seconds", 0}}),
                startup_item_json(2, "run:user:Steam", false,
                                  Json{{"source", "diagnostics"},
                                       {"cpu_seconds", 0},
                                       {"io_bytes", 0},
                                       {"logons", 0},
                                       {"delay_seconds", 3.4}}),
                startup_item_json(3, "runonce:user:Setup", nullptr, nullptr)}}};
}

inline Json vhdx_json() {
  return Json{{"state", "ready"},
              {"scanned_at", now_seconds},
              {"error", nullptr},
              {"disks",
               {{{"id", 31},
                 {"path", "D:\\WSL\\ext4.vhdx"},
                 {"area", "wsl/ubuntu"},
                 {"label", "WSL Ubuntu"},
                 {"distribution", "Ubuntu"},
                 {"size", 80'000},
                 {"size_on_disk", 70'000},
                 {"in_use", true}}}},
              {"total", 80'000},
              {"notes", {"Docker Desktop is not installed"}}};
}

inline Json dev_drive_json() {
  return Json{{"state", "ready"},
              {"scanned_at", now_seconds},
              {"error", nullptr},
              {"supported", true},
              {"build", "26200.7019"},
              {"dev_drives", {{{"volume", "Q:"}, {"file_system", "ReFS"}, {"trusted", true}}}},
              {"explanation", "A Dev Drive is a ReFS volume."},
              {"repositories", {{{"path", "F:\\src\\a"}, {"bytes", 10}, {"changed", now_seconds}}}},
              {"caches", Json::array()},
              {"total_bytes", 10},
              {"recommended_bytes", 50'000'000'000},
              {"host", {{"volume", "D:"}, {"free", 1'000'000'000'000}, {"drive_class", "nvme"}}},
              {"steps", {"Create the Dev Drive", "Move the repositories"}},
              {"notes", {"pip: no cache found"}},
              {"sources", Json::array()}};
}

inline Json boot_times_json() {
  return Json{
      {"source", "diagnostics"},
      {"log_error", nullptr},
      {"latest",
       {{"start", now_seconds},
        {"boot_seconds", 41.5},
        {"main_path_seconds", 20.0},
        {"post_boot_seconds", 21.5},
        {"logon_seconds", 30.0},
        {"boot_difference_seconds", 2.5},
        {"logon_difference_seconds", nullptr},
        {"logon", now_seconds + 60},
        {"contributors",
         {{{"image", "discord.exe"},
           {"cpu_seconds", 3.0},
           {"io_bytes", 1000},
           {"item", {{"name", "Discord"}, {"source", "run"}, {"scope", "user"}}}},
          {{"image", "svchost.exe"}, {"cpu_seconds", 1.0}, {"io_bytes", 10}, {"item", nullptr}}}}}},
      {"median", {{"boot_seconds", 39.0}, {"logon_seconds", 28.0}, {"boots", 12}}},
      {"changes",
       {{{"plan", 4},
         {"applied", now_seconds - 86400},
         {"items", {{{"name", "Discord"}, {"source", "run"}, {"scope", "user"}}}},
         {"before", {{"logon_seconds", 35.0}, {"boots", 5}}},
         {"after", nullptr}}}}};
}

inline Json apps_json() {
  return Json{{"apps",
               {{{"id", 1},
                 {"kind", "win32"},
                 {"name", "Old Tool"},
                 {"publisher", "Tools Inc"},
                 {"version", "2.1"},
                 {"install_date", "2024-01-02"},
                 {"size", 123'456'789},
                 {"description", "A disk tool"},
                 {"category", nullptr},
                 {"last_used",
                  {{"time", now_seconds - (200 * day_seconds)},
                   {"days_ago", 200},
                   {"source", "Prefetch"},
                   {"confidence", "high"},
                   {"text", "200 days ago (Prefetch)"}}},
                 {"verdict", "removable"},
                 {"reason", "the knowledge base marks it safe to remove"},
                 {"uninstaller", true}},
                {{"id", 2},
                 {"kind", "appx"},
                 {"name", "Runtime"},
                 {"publisher", nullptr},
                 {"version", nullptr},
                 {"install_date", nullptr},
                 {"size", nullptr},
                 {"description", "unknown"},
                 {"category", nullptr},
                 {"last_used",
                  {{"time", nullptr},
                   {"days_ago", nullptr},
                   {"source", nullptr},
                   {"confidence", "low"},
                   {"text", "no usage recorded in the 40 days of MyClean's history"}}},
                 {"verdict", "keep"},
                 {"reason", "a runtime"},
                 {"uninstaller", true}}}},
              {"count", 2},
              {"unused_days", nullptr},
              {"history", {{"since", now_seconds - (40 * day_seconds)}, {"days", 40}}},
              {"manufacturer", "ASUS"},
              {"problems", Json::array()}};
}

inline Json persistence_json(std::int64_t id, const char* source, const char* name,
                             const char* status, Json verdict, Json virustotal) {
  return Json{{"id", id},
              {"source", source},
              {"location", R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Run)"},
              {"name", name},
              {"command", std::string(name) + ".exe"},
              {"target", std::string(name) + ".exe"},
              {"image", std::string(R"(C:\Users\o\AppData\)") + name + ".exe"},
              {"enabled", true},
              {"signer", nullptr},
              {"startup_item", nullptr},
              {"trust", nullptr},
              {"status", status},
              {"reasons", std::string(status) == "suspicious" ? Json{"unsigned"} : Json::array()},
              {"summary", std::string(status) == "suspicious" ? "suspicious because unsigned" : ""},
              {"verdict", std::move(verdict)},
              {"virustotal", std::move(virustotal)}};
}

inline Json security_scan_json() {
  return Json{
      {"items",
       {persistence_json(1, "winlogon", "Shell", "default", nullptr, nullptr),
        persistence_json(2, "run", "agent", "suspicious", nullptr,
                         Json{{"flagged", 0}, {"engines", 0}, {"time", now_seconds}}),
        persistence_json(3, "task", "bad", "known_dangerous",
                         Json{{"source", "Microsoft Defender"},
                              {"threat", "Trojan:Win32/X"},
                              {"flagged", nullptr},
                              {"engines", nullptr},
                              {"time", now_seconds}},
                         nullptr)}},
      {"counts", {{"default", 1}, {"ok", 0}, {"suspicious", 1}, {"known_dangerous", 1}}},
      {"defender",
       {{"available", true},
        {"error", nullptr},
        {"detections",
         {{{"threat", "Trojan:Win32/X"},
           {"resources", {"file:_C:\\bad.exe"}},
           {"time", now_seconds}}}}}},
      {"virustotal", {{"enabled", false}, {"note", "off: no security.toml"}, {"queued", 0}}},
      {"problems", Json::array()}};
}

inline Json backups_json() {
  return Json{{"state", "ready"},
              {"checked_at", now_seconds},
              {"error", nullptr},
              {"sources",
               {{{"source", "backup_drives"},
                 {"name", "Backup drives"},
                 {"status", "found"},
                 {"location", "K:"},
                 {"newest", now_seconds - (90 * day_seconds)},
                 {"newest_age_days", 90},
                 {"copies", 1},
                 {"counts_toward_age", true},
                 {"detail", nullptr},
                 {"artifacts",
                  {{{"kind", "macrium_image"},
                    {"path", "K:\\Images\\c.mrimg"},
                    {"newest", now_seconds - (90 * day_seconds)}}}}},
                {{"source", "file_history"},
                 {"name", "File History"},
                 {"status", "could_not_check"},
                 {"location", nullptr},
                 {"newest", nullptr},
                 {"newest_age_days", nullptr},
                 {"copies", 0},
                 {"counts_toward_age", true},
                 {"detail", "Config1.xml: access denied"}}}},
              {"last_backup",
               {{"source", "backup_drives"},
                {"name", "Backup drives"},
                {"location", "K:"},
                {"newest", now_seconds - (90 * day_seconds)},
                {"age_days", 90}}},
              {"backup_exists", true},
              {"summary", "The newest backup is 90 days old."},
              {"recommendation", nullptr},
              {"repositories",
               {{"checked", 12},
                {"unprotected",
                 {{{"path", "F:\\src\\toy"},
                   {"no_remote", true},
                   {"unpushed", 0},
                   {"uncommitted", false},
                   {"text", "no remote"}}}},
                {"problems", Json::array()},
                {"truncated", false}}}};
}

inline Json audit_json() {
  return Json{{"entries",
               {{{"id", 42},
                 {"time", now_seconds},
                 {"plan", 12},
                 {"item", nullptr},
                 {"actor", "owner"},
                 {"event", "transition"},
                 {"from", "awaiting_approval"},
                 {"to", "approved"},
                 {"outcome", "accepted"},
                 {"reason", ""},
                 {"detail", ""},
                 {"metrics", ""}},
                {{"id", 41},
                 {"time", now_seconds - 60},
                 {"plan", 11},
                 {"item", 3},
                 {"actor", "owner"},
                 {"event", "apply"},
                 {"from", nullptr},
                 {"to", nullptr},
                 {"outcome", "deleted"},
                 {"reason", ""},
                 {"detail", ""},
                 {"metrics", "freed=4096"}}}},
              {"next", 41}};
}

inline Json reports_list_json() {
  return Json{{"reports",
               {{{"month", "2026-11"},
                 {"snapshot", "2026-11"},
                 {"generated_at", now_seconds},
                 {"against", "2026-10"},
                 {"first_report", false},
                 {"files", {{"markdown", "2026-11.md"}, {"html", "2026-11.html"}}}},
                {{"month", "2026-10"},
                 {"snapshot", "2026-10"},
                 {"generated_at", now_seconds - (30 * day_seconds)},
                 {"against", nullptr},
                 {"first_report", true},
                 {"files", {{"markdown", "2026-10.md"}, {"html", "2026-10.html"}}}}}}};
}

// One answer of every screen method, one of them failed, and a proposal that failed.
inline ScreensPoll screens_poll() {
  ScreensPoll poll;
  const auto answer = [&poll](const char* method, Result<Json> reply) {
    poll.answers.push_back(ScreenAnswer{.method = method, .reply = std::move(reply)});
  };
  answer("system_space.status", system_space_json());
  answer("space.indexed_folders", Json{{"state", "scanning"},
                                       {"scanned_at", nullptr},
                                       {"error", nullptr},
                                       {"folders",
                                        {{{"id", 7},
                                          {"path", R"(F:\src\a\target)"},
                                          {"category", "build-artifacts"},
                                          {"label", "Rust build output"}}}}});
  answer("space.vhdx", vhdx_json());
  answer("space.dev_drive", dev_drive_json());
  answer("resident.costs", fail(Errc::invalid_argument, "json-rpc error -32601: method not found"));
  answer("startup.list", startup_json());
  answer("startup.boot_times", boot_times_json());
  answer("apps.list", apps_json());
  answer("security.scan", security_scan_json());
  answer("security.extensions", Json{{"extensions", Json::array()}, {"problems", Json::array()}});
  answer("security.network", Json{{"hours", 24}, {"processes", Json::array()}});
  answer("backups.check", backups_json());
  answer("audit.list", audit_json());
  answer("reports.list", reports_list_json());
  answer("reports.get", Json{{"found", true}, {"month", "2026-11"}, {"markdown", "# November"}});
  poll.action = ActionOutcome{.text = "Proposing the setting failed: hibernation needs no change",
                              .failed = true};
  return poll;
}

}  // namespace myclean::tray_model::testing
