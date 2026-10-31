#include "core/ipc/status.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"

namespace myclean::ipc {

namespace {

using Json = nlohmann::json;

Json optional_text(const std::optional<std::string>& text) {
  return text ? Json(*text) : Json(nullptr);
}

std::optional<std::string> parse_optional_text(const Json& value) {
  return value.is_null() ? std::nullopt : std::optional<std::string>(value.get<std::string>());
}

}  // namespace

Json to_json(const StatusReply& status) {
  return Json{
      {"health", status.healthy ? "ok" : "degraded"},
      {"version", optional_text(status.version)},
      {"build", status.build},
      {"mode", status.mode},
      {"pid", status.pid},
      {"uptime_s", status.uptime_seconds},
      {"store", Json{{"path", status.store.path}, {"error", optional_text(status.store.error)}}},
      {"components", status.components}};
}

Result<StatusReply> parse_status(const Json& value) {
  try {
    const std::string health = value.at("health").get<std::string>();
    if (health != "ok" && health != "degraded") {
      return fail(Errc::invalid_data, "status: unknown health '" + health + "'");
    }
    const Json& store = value.at("store");
    return StatusReply{.healthy = health == "ok",
                       .version = parse_optional_text(value.at("version")),
                       .build = value.at("build").get<std::string>(),
                       .mode = value.at("mode").get<std::string>(),
                       .pid = value.at("pid").get<std::int64_t>(),
                       .uptime_seconds = value.at("uptime_s").get<std::int64_t>(),
                       .store = StoreStatus{.path = store.at("path").get<std::string>(),
                                            .error = parse_optional_text(store.at("error"))},
                       .components = value.at("components").get<std::vector<std::string>>()};
  } catch (const Json::exception& error) {
    return fail(Errc::invalid_data, std::string("status: ") + error.what());
  }
}

}  // namespace myclean::ipc
