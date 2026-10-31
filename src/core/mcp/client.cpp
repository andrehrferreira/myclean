#include "core/mcp/client.hpp"

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/ipc/telemetry.hpp"

namespace myclean::mcp {

namespace {

using Json = nlohmann::json;

// metrics.latest values derived for prompt 1.
Json signals(const Json& latest) {
  Json result{{"commit_percent", nullptr},
              {"system_volume", nullptr},
              {"system_volume_free_percent", nullptr},
              {"disk_queue", nullptr}};
  const Json& memory = latest.value("memory", Json::object());
  if (memory.contains("commit") && memory.value("commit_limit", 0.0) > 0.0) {
    result["commit_percent"] =
        100.0 * memory.at("commit").get<double>() / memory.at("commit_limit").get<double>();
  }
  for (const Json& volume : latest.value("volumes", Json::array())) {
    if (volume.value("system", false)) {
      result["system_volume"] = volume.at("volume");
      result["system_volume_free_percent"] = volume.at("free_percent");
    }
  }
  const Json& disks = latest.value("disks", Json::array());
  if (!disks.empty()) {
    double queue = 0.0;
    for (const Json& disk : disks) {
      queue += disk.value("queue", 0.0);
    }
    result["disk_queue"] = queue;
  }
  return result;
}

}  // namespace

std::string unavailable_text(std::string_view reason) {
  return "The MyClean service is not reachable (" + std::string(reason) +
         "), so live telemetry (CPU, memory, disks, GPU, process trees), history, volumes and "
         "disk health are unavailable. Start the MyClean service and try again.";
}

Reply health_snapshot(const Caller& call) {
  Reply top =
      call(ipc::processes_top_method, Json{{"qualifying", true}, {"limit", ipc::max_top_trees}});
  if (!top) {
    return top;
  }
  Reply latest = call(ipc::metrics_latest_method, Json::object());
  if (!latest) {
    return latest;
  }
  Json snapshot = std::move(*latest);
  snapshot["signals"] = signals(snapshot);
  snapshot.update(*top);
  return snapshot;
}

}  // namespace myclean::mcp
