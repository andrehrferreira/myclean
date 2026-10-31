#pragma once

#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

// The service side of the input watchdog, which itself runs in the tray: the input_incident
// table (kept 2 years) and the methods input.record_incident and input.summary
// (core/ipc/input.hpp). The incident arrives as a record parameter, checked against the strict
// schema of core/input/incident_record.hpp, and is only stored.
class InputComponent final : public IComponent {
 public:
  InputComponent() = default;

  [[nodiscard]] std::string_view name() const override { return "input"; }
  [[nodiscard]] std::vector<store::MigrationSet> migrations() const override;
  [[nodiscard]] Result<void> start(const Context& context) override;
  void stop() override {}

  [[nodiscard]] Result<nlohmann::json> record(const nlohmann::json& params) const;
  [[nodiscard]] Result<nlohmann::json> summary() const;

 private:
  store::Store* store_ = nullptr;
};

}  // namespace myclean::svc
