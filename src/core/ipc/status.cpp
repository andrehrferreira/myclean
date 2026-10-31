#include "core/ipc/status.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

std::string_view health_name(bool healthy) {
  return healthy ? "ok" : "degraded";
}

Result<bool> parse_health(const Json& value) {
  const std::string health = value.at("health").get<std::string>();
  if (health != "ok" && health != "degraded") {
    return fail(Errc::invalid_data, "status: unknown health '" + health + "'");
  }
  return health == "ok";
}

}  // namespace

Json to_json(const StatusReply& status) {
  Json components = Json::array();
  for (const ComponentHealth& component : status.component_health) {
    components.push_back(Json{{"name", component.name},
                              {"health", health_name(component.healthy)},
                              {"problems", component.problems},
                              {"unavailable", component.unavailable}});
  }
  return Json{
      {"health", health_name(status.healthy)},
      {"version", optional_text(status.version)},
      {"build", status.build},
      {"mode", status.mode},
      {"pid", status.pid},
      {"uptime_s", status.uptime_seconds},
      {"store", Json{{"path", status.store.path}, {"error", optional_text(status.store.error)}}},
      {"components", status.components},
      {"component_health", std::move(components)}};
}

Result<StatusReply> parse_status(const Json& value) {
  try {
    const Result<bool> healthy = parse_health(value);
    if (!healthy) {
      return tl::unexpected(healthy.error());
    }
    std::vector<ComponentHealth> components;
    for (const Json& component : value.value("component_health", Json::array())) {
      const Result<bool> component_healthy = parse_health(component);
      if (!component_healthy) {
        return tl::unexpected(component_healthy.error());
      }
      components.push_back(ComponentHealth{
          .name = component.at("name").get<std::string>(),
          .healthy = *component_healthy,
          .problems = component.at("problems").get<std::vector<std::string>>(),
          .unavailable = component.at("unavailable").get<std::vector<std::string>>()});
    }
    const Json& store = value.at("store");
    return StatusReply{.healthy = *healthy,
                       .version = parse_optional_text(value.at("version")),
                       .build = value.at("build").get<std::string>(),
                       .mode = value.at("mode").get<std::string>(),
                       .pid = value.at("pid").get<std::int64_t>(),
                       .uptime_seconds = value.at("uptime_s").get<std::int64_t>(),
                       .store = StoreStatus{.path = store.at("path").get<std::string>(),
                                            .error = parse_optional_text(store.at("error"))},
                       .components = value.at("components").get<std::vector<std::string>>(),
                       .component_health = std::move(components)};
  } catch (const Json::exception& error) {
    return fail(Errc::invalid_data, std::string("status: ") + error.what());
  }
}

}  // namespace myclean::ipc
