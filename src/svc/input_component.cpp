#include "svc/input_component.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/input/classifier.hpp"
#include "core/input/incident.hpp"
#include "core/input/incident_record.hpp"
#include "core/input/incident_store.hpp"
#include "core/ipc/input.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/model/result.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

namespace {

using Json = nlohmann::json;

Json stored_json(const input::StoredIncident& stored) {
  Json incident = input::to_record(stored.incident);
  incident["id"] = stored.id;
  const auto& cause = stored.classification.process;
  incident["cause"] = cause ? Json{{"pid", cause->pid}, {"image", cause->image}} : Json(nullptr);
  incident["evidence"] = stored.classification.evidence;
  return incident;
}

}  // namespace

std::vector<store::MigrationSet> InputComponent::migrations() const {
  return {input::input_migrations()};
}

Result<void> InputComponent::start(const Context& context) {
  store_ = &context.store();
  if (Result<void> retention = store_->add_retention(input::input_retention()); !retention) {
    return retention;
  }
  using ipc::Param;
  using ipc::ParamType;
  // The only record parameter of the service: it is validated against the strict incident
  // schema before the handler runs, and its strings are stored, never used as OS input.
  const ipc::RecordSchema incident_schema = [](const Json& record) {
    const Result<input::Incident> incident = input::parse_record(record);
    return incident ? std::string{} : incident.error().message;
  };
  return context.methods()
      .add(std::string(ipc::input_record_incident_method),
           {Param{.name = "incident",
                  .type = ParamType::record,
                  .required = true,
                  .schema = incident_schema}},
           [this](const Json& params) { return record(params); })
      .and_then([&] {
        return context.methods().add(std::string(ipc::input_summary_method), {},
                                     [this](const Json&) { return summary(); });
      });
}

Result<Json> InputComponent::record(const Json& params) const {
  return input::parse_record(params.at("incident"))
      .and_then([this](const input::Incident& incident) {
        return input::save_incident(*store_, incident);
      })
      .transform([](std::int64_t id) { return Json{{"id", id}}; });
}

Result<Json> InputComponent::summary() const {
  return input::freeze_summary(*store_).transform(
      [](const std::vector<input::FreezeGroup>& groups) {
        Json list = Json::array();
        for (const input::FreezeGroup& group : groups) {
          Json incidents = Json::array();
          for (std::size_t index = 0;
               index < group.incidents.size() && index < ipc::max_summary_incidents; ++index) {
            incidents.push_back(stored_json(group.incidents[index]));
          }
          list.push_back(Json{{"mechanism", input::to_string(group.mechanism)},
                              {"process", group.process ? Json(*group.process) : Json(nullptr)},
                              {"count", group.incidents.size()},
                              {"latest", store::to_unix_seconds(group.latest)},
                              {"incidents", std::move(incidents)}});
        }
        return Json{{"groups", std::move(list)}};
      });
}

}  // namespace myclean::svc
