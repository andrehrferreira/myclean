#pragma once

#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/safety/plan_source.hpp"
#include "core/store/store.hpp"

namespace myclean::actions {

// The plans SafeRemove may act on: those stored in the store's plan tables.
class StorePlanSource final : public safety::IPlanSource {
 public:
  // `store` must outlive the source.
  explicit StorePlanSource(store::Store& store) : store_(&store) {}

  [[nodiscard]] Result<Plan> load(PlanId id) const override;

 private:
  store::Store* store_;
};

}  // namespace myclean::actions
