#pragma once

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/report/snapshot.hpp"

// The service's read methods as the monthly snapshot calls them, answered from fixtures of one
// machine: Discord at logon, the Acme service, one Chrome extension, svchost listening on TCP 135,
// an unsigned updater task, a build tree that averaged 40 % CPU over 30 days (and is idle now),
// one healthy NVMe drive and C: with 200 GB free.
namespace myclean::report::testing {

using Json = nlohmann::json;

inline Json trust(std::string_view status, std::string_view path) {
  return Json{{"status", status}, {"path", path}};
}

class FakeService {
 public:
  FakeService() {
    answers_["volumes.list"] = Json{{"volumes", Json::array({{{"volume", "C:"},
                                                              {"entity", "C"},
                                                              {"size", 1'000'000'000'000},
                                                              {"free", 200'000'000'000},
                                                              {"system", true}}})}};
    answers_["space.top"] = Json{
        {"state", "ready"},
        {"folders",
         Json::array(
             {{{"path", R"(C:\Users\owner\build)"},
               {"size_on_disk", 50'000'000'000},
               {"files", 1000}},
              {{"path", R"(C:\Windows)"}, {"size_on_disk", 30'000'000'000}, {"files", 5000}}})},
        {"files",
         Json::array({{{"path", R"(C:\pagefile.sys)"}, {"size_on_disk", 16'000'000'000}}})}};
    answers_["space.old_downloads"] =
        Json{{"state", "ready"},
             {"files", Json::array({{{"size_on_disk", 1000}}, {{"size_on_disk", 2000}}})}};
    answers_["space.duplicates"] = Json{
        {"state", "ready"},
        {"groups",
         Json::array({{{"files", Json::array({Json::object(), Json::object(), Json::object()})},
                       {"reclaimable", 500}}})},
        {"reclaimable", 500}};
    answers_["space.used_installers"] =
        Json{{"state", "ready"}, {"used", Json::array({{{"size_on_disk", 300}}})}};
    answers_["startup.list"] = Json{{"items", Json::array({{{"key", "run:user:Discord"},
                                                            {"source", "run"},
                                                            {"scope", "user"},
                                                            {"name", "Discord"},
                                                            {"display_name", "Discord"},
                                                            {"command", "Discord.exe --start"},
                                                            {"enabled", true}},
                                                           {{"key", "service:machine:AcmeSvc"},
                                                            {"source", "service"},
                                                            {"scope", "machine"},
                                                            {"name", "AcmeSvc"},
                                                            {"display_name", "Acme Service"},
                                                            {"command", R"(C:\Acme\svc.exe)"},
                                                            {"signer", "Acme Inc."},
                                                            {"enabled", true}}})}};
    answers_["security.extensions"] =
        Json{{"extensions", Json::array({extension("xyz", "Docs", nullptr)})},
             {"problems", Json::array()}};
    answers_["security.network"] =
        Json{{"processes",
              Json::array({{{"pid", 10},
                            {"image", "svchost.exe"},
                            {"visible_window", false},
                            {"sent", 10},
                            {"received", 20},
                            {"trust", trust("signed", R"(C:\Windows\System32\svchost.exe)")},
                            {"listening", Json::array({{{"protocol", "tcp"}, {"port", 135}}})}}})}};
    answers_["security.scan"] =
        Json{{"items",
              Json::array({{{"name", "Updater"},
                            {"source", "task"},
                            {"status", "suspicious"},
                            {"summary", "suspicious because unsigned"},
                            {"trust", trust("unsigned", R"(C:\Users\owner\AppData\up.exe)")}}})},
             {"counts", {{"default", 5}, {"ok", 3}, {"suspicious", 1}, {"known_dangerous", 0}}},
             {"defender", {{"available", true}, {"detections", Json::array()}}}};
    answers_["processes.top"] =
        Json{{"trees", Json::array({{{"pid", 1},
                                     {"image", "cl.exe"},
                                     {"cpu_percent", 0.1},
                                     {"trust", trust("signed", R"(C:\VS\cl.exe)")}}})}};
    answers_["disks.health"] = Json{{"drives", Json::array({drive("good", 0)})}};
    answers_["metrics.latest"] = Json{{"cpu", {{"logical_processors", 4}}}};
    // 40 % of 4 processors for the 100 hours it ran.
    answers_["resident.costs"] =
        Json{{"days", 30},
             {"programs", Json::array({{{"image", "cl.exe"},
                                        {"key", "cl.exe"},
                                        {"hours_running", 100.0},
                                        {"cpu_seconds", 0.4 * 100 * 3600 * 4},
                                        {"ram_average", 1'000'000'000},
                                        {"ram_peak", 2'000'000'000}}})}};
  }

  static Json extension(std::string_view id, std::string_view name, const Json& flag) {
    return Json{{"browser", "chrome"}, {"profile", "Default"}, {"id", id},        {"name", name},
                {"version", "1.0"},    {"origin", "store"},    {"enabled", true}, {"flag", flag}};
  }

  static Json drive(std::string_view status, int media_errors) {
    return Json{{"key", "Samsung_1"},
                {"model", "Samsung 990 PRO"},
                {"status", status},
                {"reasons", Json::array()},
                {"temperature_c", 40},
                {"percent_used", 3},
                {"available_spare", 100},
                {"media_errors", media_errors},
                {"reallocated_sectors", nullptr},
                {"pending_sectors", nullptr},
                {"power_on_hours", 1200},
                {"data_written_bytes", 5'000'000'000'000}};
  }

  Result<Json> call(std::string_view method, const Json& params) {
    calls_.emplace_back(std::string(method), params);
    if (const auto failure = failures_.find(std::string(method)); failure != failures_.end()) {
      return fail(Errc::io_error, failure->second);
    }
    if (method.starts_with("space.") && scanning_ > 0) {
      --scanning_;
      return Json{{"state", "scanning"}};
    }
    if (method == "metrics.query") {
      return history(params.value("metric", ""));
    }
    const auto found = answers_.find(std::string(method));
    if (found == answers_.end()) {
      return fail(Errc::not_found, "json-rpc error -32601: method not found");
    }
    return found->second;
  }

  [[nodiscard]] Call caller() {
    return [this](std::string_view method, const Json& params) { return call(method, params); };
  }

  Json& answer(const std::string& method) { return answers_[method]; }
  void fail_method(const std::string& method, std::string message) {
    failures_[method] = std::move(message);
  }
  // The next `polls` calls of a space.* method answer that the analysis still runs.
  void keep_scanning(int polls) { scanning_ = polls; }
  [[nodiscard]] const std::vector<std::pair<std::string, Json>>& calls() const { return calls_; }

 private:
  static Json history(std::string_view metric) {
    if (metric == "volume.free") {
      return Json{
          {"points", Json::array({{{"time", 1'790'000'000}, {"mean", 210'000'000'000.0}},
                                  {{"time", 1'790'086'400}, {"mean", 200'000'000'000.0}}})}};
    }
    if (metric == "cpu.utility") {
      return Json{
          {"points", Json::array({{{"time", 1'790'000'000}, {"mean", 22.0}, {"max", 95.0}}})}};
    }
    return Json{{"points", Json::array({{{"time", 1'790'000'000},
                                         {"mean", 8'000'000'000.0},
                                         {"min", 2'000'000'000.0}}})}};
  }

  std::map<std::string, Json> answers_;
  std::map<std::string, std::string> failures_;
  std::vector<std::pair<std::string, Json>> calls_;
  int scanning_ = 0;
};

}  // namespace myclean::report::testing
