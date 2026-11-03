#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/tray_model/poller.hpp"

namespace myclean::tray_model::testing {

using Json = nlohmann::json;

inline constexpr double gib = 1024.0 * 1024.0 * 1024.0;

// A metrics.latest reply in the service's shape (core/ipc/telemetry.hpp) for Unix second `time`:
// 4 logical processors ("0_0".."0_3" at 10, 20, 30 and 40 %, each at 70 % of the maximum
// frequency), disks "0", "2" and "10", one GPU adapter, C: (system, filling) and D: (not
// filling).
inline Json latest_json(std::int64_t time) {
  const auto disk = [](std::string_view number, double read) {
    return Json{{"disk", number}, {"read_bytes", read},  {"write_bytes", 2.0e6},
                {"iops", 150.0},  {"read_latency", 0.4}, {"write_latency", 1.2},
                {"queue", 1.0},   {"idle", 97.0}};
  };
  return Json{
      {"time", time},
      {"cpu",
       {{"utility", 42.0},
        {"frequency", 87.0},
        {"dpc", 0.5},
        {"interrupt", 0.3},
        {"queue", 2.0},
        {"logical_processors", 4},
        {"processors",
         Json::array({Json{{"entity", "0_0"}, {"utility", 10.0}, {"frequency", 70.0}},
                      Json{{"entity", "0_1"}, {"utility", 20.0}, {"frequency", 70.0}},
                      Json{{"entity", "0_2"}, {"utility", 30.0}, {"frequency", 70.0}},
                      Json{{"entity", "0_3"}, {"utility", 40.0}, {"frequency", 70.0}}})}}},
      {"memory",
       {{"total", 64 * gib},
        {"available", 40 * gib},
        {"commit", 30 * gib},
        {"commit_limit", 72 * gib},
        {"standby", 10 * gib},
        {"modified", 1 * gib},
        {"compressed", 0.5 * gib},
        {"pool_paged", 0.8 * gib},
        {"pool_nonpaged", 0.3 * gib},
        {"hard_faults", 12.0}}},
      {"process", {{"count", 300}, {"threads", 4000}, {"handles", 150000}}},
      {"disks", Json::array({disk("0", 1.0e6), disk("10", 3.0e6), disk("2", 5.0e6)})},
      {"gpu",
       {{"adapters", Json::array({Json{{"adapter", "luid_0x0_0xD1B5_phys_0"},
                                       {"entity", "luid_0x0_0xD1B5_phys_0"},
                                       {"utilization", 12.0},
                                       {"dedicated", 2 * gib},
                                       {"shared", 0.25 * gib},
                                       {"engines", {{"3D", 12.0}, {"Copy", 1.0}}}}})}}},
      {"volumes",
       Json::array(
           {Json{{"volume", "C:"},
                 {"entity", "C"},
                 {"mount_points", {"C:\\"}},
                 {"size", 952ULL << 30U},
                 {"free", 84ULL << 30U},
                 {"free_percent", 8.8},
                 {"disks", {0}},
                 {"system", true},
                 {"fill",
                  {{"state", "filling"}, {"bytes_per_day", -2.0e9}, {"days_until_full", 40.5}}}},
            Json{{"volume", "D:"},
                 {"entity", "D"},
                 {"mount_points", {"D:\\"}},
                 {"size", 2000ULL << 30U},
                 {"free", 1500ULL << 30U},
                 {"free_percent", 75.0},
                 {"disks", {2, 10}},
                 {"system", false},
                 {"fill",
                  {{"state", "not_filling"},
                   {"bytes_per_day", 0.0},
                   {"days_until_full", nullptr}}}}})},
      {"unavailable", Json::array()}};
}

// A metrics.query reply with one point every `step` seconds in [from, to), each of value
// `value`.
inline Json query_json(std::string_view metric, std::string_view entity, std::int64_t from,
                       std::int64_t to, double value, std::int64_t step = 1) {
  Json points = Json::array();
  for (std::int64_t time = from; time < to; time += step) {
    points.push_back(
        Json{{"time", time}, {"min", value}, {"max", value}, {"mean", value}, {"count", 1}});
  }
  return Json{
      {"metric", metric}, {"entity", entity}, {"resolution", "raw"}, {"points", std::move(points)}};
}

// A trust signal: a signed image the knowledge base lists, or an unsigned unrecognized one in a
// user-writable folder.
inline Json trust_json(bool recognized) {
  if (recognized) {
    return Json{{"status", "signed"},
                {"signer", "Browser Vendor Inc."},
                {"path", R"(C:\Program Files\Browser\browser.exe)"},
                {"path_class", "protected"},
                {"unrecognized", false},
                {"reasons", Json::array()},
                {"description", "Web browser"},
                {"category", "browser"}};
  }
  return Json{{"status", "unsigned"},
              {"signer", nullptr},
              {"path", R"(C:\Users\owner\AppData\Local\tool.exe)"},
              {"path_class", "user_writable"},
              {"unrecognized", true},
              {"reasons", {"not-in-knowledge-base", "unsigned", "user-writable-path"}},
              {"description", nullptr},
              {"category", nullptr}};
}

// A processes.top reply: a browser tree of 3 processes (signed, known) and a single process
// (unsigned, unrecognized).
inline Json top_json() {
  const auto member = [](std::uint32_t pid, std::string_view image, double cpu) {
    return Json{{"pid", pid},
                {"parent_pid", 1},
                {"image", image},
                {"cpu_percent", cpu},
                {"private_working_set", 100 << 20},
                {"private_bytes", 120 << 20}};
  };
  const auto tree = [](std::uint32_t pid, std::string_view image, double cpu, Json members,
                       Json trust) {
    return Json{{"pid", pid},
                {"image", image},
                {"processes", members.size()},
                {"cpu_percent", cpu},
                {"memory_percent", 1.5},
                {"private_working_set", 300 << 20},
                {"private_bytes", 360 << 20},
                {"working_set", 400 << 20},
                {"io_bytes_per_second", 0.0},
                {"handles", 900},
                {"threads", 80},
                {"members", std::move(members)},
                {"trust", std::move(trust)}};
  };
  return Json{
      {"time", 1000},
      {"logical_processors", 4},
      {"memory_total", 64ULL << 30U},
      {"filter", nullptr},
      {"total_trees", 2},
      {"trees", Json::array({tree(100, "browser.exe", 9.0,
                                  Json::array({member(100, "browser.exe", 5.0),
                                               member(101, "browser.exe", 3.0),
                                               member(102, "gpu-helper.exe", 1.0)}),
                                  trust_json(true)),
                             tree(200, "tool.exe", 1.0, Json::array({member(200, "tool.exe", 1.0)}),
                                  trust_json(false))})}};
}

// A processes.explain reply for PID 101 of the browser tree.
inline Json explain_json() {
  return Json{{"pid", 101},
              {"parent_pid", 100},
              {"image", "browser.exe"},
              {"path", R"(C:\Program Files\Browser\browser.exe)"},
              {"cpu_percent", 3.0},
              {"memory_percent", 0.2},
              {"private_working_set", 100 << 20},
              {"private_bytes", 120 << 20},
              {"working_set", 140 << 20},
              {"tree",
               {{"pid", 100},
                {"image", "browser.exe"},
                {"processes", 3},
                {"cpu_percent", 9.0},
                {"memory_percent", 1.5}}},
              {"members", top_json()["trees"][0]["members"]},
              {"trust", trust_json(true)},
              {"kb",
               {{"description", "Web browser"},
                {"category", "browser"},
                {"needed_at_startup", false},
                {"safe_to_remove", false}}}};
}

// A disks.health reply: disk "0" an NVMe drive in warning (a media error, above its rated
// temperature), disk "2" a SATA drive in good health without a rated limit, disk "7" a USB drive
// whose bridge blocks SMART, with the reliability counters Windows provides.
inline Json health_json() {
  const auto endurance = [](std::string_view state, Json years, std::string_view note) {
    const bool projected = state == "projected";
    return Json{{"state", state},
                {"years_left", std::move(years)},
                {"tbw_rated", projected ? Json(1200.0) : Json(nullptr)},
                {"tbw_used_percent", projected ? Json(25.0) : Json(nullptr)},
                {"note", note}};
  };
  const auto drive = [&endurance](std::string_view disk, std::string_view model,
                                  std::string_view bus) {
    return Json{{"key", std::string(model.substr(0, 3)) + "_1A2B"},
                {"disk", disk},
                {"model", model},
                {"bus", bus},
                {"source", "nvme_health_log"},
                {"volumes", Json::array()},
                {"status", "good"},
                {"reasons", Json::array()},
                {"readable", true},
                {"unreadable_reason", nullptr},
                {"temperature_c", 41},
                {"temperature_limit_c", 70},
                {"percent_used", 3},
                {"available_spare", 100},
                {"media_errors", 0},
                {"reallocated_sectors", nullptr},
                {"pending_sectors", nullptr},
                {"power_on_hours", 5000},
                {"data_written_bytes", 300'000'000'000'000LL},
                {"endurance", endurance("projected", 18.0, "")}};
  };
  Json nvme = drive("0", "Samsung SSD 980 PRO 2TB", "nvme");
  nvme["volumes"] = {"C:"};
  nvme["status"] = "warning";
  nvme["reasons"] = {"1 media error", "74 °C is above the rated 70 °C"};
  nvme["temperature_c"] = 74;
  nvme["media_errors"] = 1;
  Json sata = drive("2", "WDC WD101EFBX", "ata");
  sata["source"] = "ata_smart";
  sata["volumes"] = {"D:"};
  for (const char* member : {"temperature_limit_c", "percent_used", "available_spare",
                             "media_errors", "data_written_bytes"}) {
    sata[member] = nullptr;
  }
  sata["reallocated_sectors"] = 0;
  sata["pending_sectors"] = 0;
  sata["endurance"] = endurance("not_reported", nullptr, "the drive reports no data written");
  Json usb = drive("7", "ROG ESD-S1C", "usb");
  usb["source"] = "reliability_counters";
  usb["status"] = "unknown";
  usb["readable"] = false;
  usb["unreadable_reason"] = "not readable over USB: the bridge blocks SMART pass-through";
  for (const char* member :
       {"temperature_limit_c", "percent_used", "available_spare", "media_errors"}) {
    usb[member] = nullptr;
  }
  usb["endurance"] = endurance("rating_unknown", nullptr, "no TBW rating is known for the model");
  return Json{{"time", 1000}, {"drives", Json::array({nvme, sata, usb})}};
}

// An alerts.badge reply.
inline Json badge_json(int count, std::string_view worst) {
  return Json{{"count", count}, {"worst", worst}};
}

// One alert of an alerts.list reply; `toast_at` is null when no toast was decided.
inline Json alert_json(std::string_view rule, std::string_view entity, std::string_view severity,
                       bool active, Json toast_at) {
  return Json{{"rule", rule},
              {"entity", entity},
              {"title", std::string(rule) + " title"},
              {"severity", severity},
              {"first_at", 900},
              {"last_at", 950},
              {"count", 3},
              {"active", active},
              {"snoozed_until", nullptr},
              {"toast_at", std::move(toast_at)},
              {"detail", std::string(entity) + " detail"},
              {"value", 9.0},
              {"limit", 10.0},
              {"previous", nullptr},
              {"unit", "%"}};
}

inline Json alerts_json(Json alerts) {
  return Json{{"alerts", std::move(alerts)}};
}

// An input.summary reply: 2 clipboard-held-open freezes caused by one process and 1 unexplained.
inline Json summary_json() {
  const auto incident = [](std::int64_t id, std::int64_t latency, Json cause) {
    return Json{{"id", id},
                {"chord", "paste"},
                {"time", 1000 - id},
                {"latency_ms", latency},
                {"capped", false},
                {"owner_unanswered_ms", nullptr},
                {"input_lag_ms", 0},
                {"foreground", {{"pid", 10}, {"image", "editor.exe"}}},
                {"owner", nullptr},
                {"holder", nullptr},
                {"viewer", nullptr},
                {"formats", Json::array()},
                {"hung", Json::array()},
                {"suspects", Json::array()},
                {"cpu_percent", 12.5},
                {"memory_load_percent", 40},
                {"cause", std::move(cause)},
                {"evidence", {"evidence of incident " + std::to_string(id)}}};
  };
  const Json holder = {{"pid", 77}, {"image", "clipmgr.exe"}};
  return Json{
      {"groups",
       Json::array(
           {Json{{"mechanism", "clipboard_held_open"},
                 {"process", "clipmgr.exe"},
                 {"count", 2},
                 {"latest", 999},
                 {"incidents", Json::array({incident(1, 600, holder), incident(2, 400, holder)})}},
            Json{{"mechanism", "unexplained"},
                 {"process", nullptr},
                 {"count", 1},
                 {"latest", 997},
                 {"incidents", Json::array({incident(3, 300, nullptr)})}}})}};
}

// One pending governor notification for the tree rooted at `pid` (created at `pid` * 1000 ns), at
// Unix second `time`.
inline Json governor_notification_json(std::int64_t id, std::int64_t time, std::uint32_t pid,
                                       std::string_view image, std::string_view step) {
  return Json{{"id", id},
              {"time", time},
              {"root", {{"pid", pid}, {"created", std::int64_t{pid} * 1000}}},
              {"image", image},
              {"class", "default"},
              {"step", step},
              {"reason", "cpu"},
              {"members", 3}};
}

// A governor.status reply in the service's shape (core/ipc/governor.hpp): act mode under CPU
// pressure, game mode on for one game, one tree at the EcoQoS step, `notifications` and two
// events.
inline Json governor_status_json(Json notifications = Json::array()) {
  return Json{{"mode", "act"},
              {"paused_until", nullptr},
              {"pressure",
               {{"state", "pressure"},
                {"clearing", false},
                {"reasons", {"cpu"}},
                {"metrics",
                 {{"cpu_percent", 97.0},
                  {"available", 20 * gib},
                  {"commit", 40 * gib},
                  {"commit_limit", 150 * gib},
                  {"hard_faults_per_second", 3.0}}}}},
              {"game_mode",
               {{"active", true},
                {"override", "auto"},
                {"build_cpu_percent", 40},
                {"games", Json::array({Json{{"pid", 900},
                                            {"created", 900000},
                                            {"image", "title.exe"},
                                            {"reason", "library_root"}}})}}},
              {"jobs", Json::array()},
              {"trees", Json::array({Json{{"root", {{"pid", 300}, {"created", 300000}}},
                                          {"image", "bigjob.exe"},
                                          {"class", "default"},
                                          {"step", "ecoqos"}}})},
              {"notifications", std::move(notifications)},
              {"events", Json::array({Json{{"id", 2},
                                           {"time", 1000},
                                           {"kind", "apply_step"},
                                           {"root", {{"pid", 300}, {"created", 300000}}},
                                           {"image", "bigjob.exe"},
                                           {"class", "default"},
                                           {"step", "ecoqos"},
                                           {"reason", "cpu"},
                                           {"actor", "governor"},
                                           {"members", 3},
                                           {"metrics", nullptr},
                                           {"detail", "3 of 3 processes"}},
                                      Json{{"id", 1},
                                           {"time", 990},
                                           {"kind", "game_mode"},
                                           {"root", nullptr},
                                           {"image", ""},
                                           {"class", nullptr},
                                           {"step", nullptr},
                                           {"reason", "policy"},
                                           {"actor", "governor"},
                                           {"members", 0},
                                           {"metrics", nullptr},
                                           {"detail", "game mode on (override auto)"}}})}};
}

// A scripted service: each method answers through its handler; methods without one answer
// "method not found". Records every call. While offline every call fails as if the pipe were
// gone.
class FakeLink final : public IServiceLink {
 public:
  using Handler = std::function<Result<Json>(const Json& params)>;

  struct Call {
    std::string method;
    Json params;
  };

  void on(std::string method, Handler handler) {
    handlers_.insert_or_assign(std::move(method), std::move(handler));
  }

  [[nodiscard]] Result<Json> call(std::string_view method, const Json& params) override {
    calls_.push_back(Call{.method = std::string(method), .params = params});
    if (!online_) {
      return fail(Errc::not_found, R"(connect to \\.\pipe\fake: the service is not running)");
    }
    const auto found = handlers_.find(std::string(method));
    if (found == handlers_.end()) {
      return fail(Errc::invalid_argument, "json-rpc error -32601: method not found");
    }
    return found->second(params);
  }

  [[nodiscard]] bool reachable() const override { return online_; }

  // Takes the service away (false) or brings it back.
  void set_online(bool online) { online_ = online; }
  [[nodiscard]] const std::vector<Call>& calls() const { return calls_; }
  void clear_calls() { calls_.clear(); }

  // The calls of `method`.
  [[nodiscard]] std::vector<Call> calls_of(std::string_view method) const {
    std::vector<Call> matching;
    for (const Call& call : calls_) {
      if (call.method == method) {
        matching.push_back(call);
      }
    }
    return matching;
  }

 private:
  bool online_ = true;
  std::vector<Call> calls_;
  std::map<std::string, Handler, std::less<>> handlers_;
};

}  // namespace myclean::tray_model::testing
