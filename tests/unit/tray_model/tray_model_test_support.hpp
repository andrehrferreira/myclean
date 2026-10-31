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
// 4 logical processors, disks "0", "2" and "10", one GPU adapter, C: (system, filling) and D:
// (not filling).
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
        {"logical_processors", 4}}},
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

// A metrics.query reply with one point per second in [from, to), each of value `value`.
inline Json query_json(std::string_view metric, std::string_view entity, std::int64_t from,
                       std::int64_t to, double value) {
  Json points = Json::array();
  for (std::int64_t time = from; time < to; ++time) {
    points.push_back(
        Json{{"time", time}, {"min", value}, {"max", value}, {"mean", value}, {"count", 1}});
  }
  return Json{
      {"metric", metric}, {"entity", entity}, {"resolution", "raw"}, {"points", std::move(points)}};
}

// A processes.top reply: a browser tree of 3 processes and a single process.
inline Json top_json() {
  const auto member = [](std::uint32_t pid, std::string_view image, double cpu) {
    return Json{{"pid", pid},
                {"parent_pid", 1},
                {"image", image},
                {"cpu_percent", cpu},
                {"private_working_set", 100 << 20},
                {"private_bytes", 120 << 20}};
  };
  const auto tree = [](std::uint32_t pid, std::string_view image, double cpu, Json members) {
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
                {"trust", nullptr}};
  };
  return Json{
      {"time", 1000},
      {"logical_processors", 4},
      {"memory_total", 64ULL << 30U},
      {"filter", nullptr},
      {"total_trees", 2},
      {"trees",
       Json::array(
           {tree(100, "browser.exe", 9.0,
                 Json::array({member(100, "browser.exe", 5.0), member(101, "browser.exe", 3.0),
                              member(102, "gpu-helper.exe", 1.0)})),
            tree(200, "tool.exe", 1.0, Json::array({member(200, "tool.exe", 1.0)}))})}};
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
