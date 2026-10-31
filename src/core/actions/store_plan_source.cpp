#include "core/actions/store_plan_source.hpp"

#include <optional>

#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/store/database.hpp"
#include "core/store/plan_repository.hpp"

namespace myclean::actions {

Result<Plan> StorePlanSource::load(PlanId id) const {
  Result<Plan> plan = fail(Errc::io_error, "plan not read");
  const Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
    plan = store::get_plan(db, id);
    return {};
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return plan;
}

}  // namespace myclean::actions
