#pragma once

#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"

namespace myclean::safety {

// Read access to the plans MyClean itself built and stored. SafeRemove resolves every request
// through it, so no caller can hand SafeRemove a path to act on. Implemented over the store.
class IPlanSource {
 public:
  virtual ~IPlanSource() = default;
  IPlanSource(const IPlanSource&) = delete;
  IPlanSource& operator=(const IPlanSource&) = delete;
  IPlanSource(IPlanSource&&) = delete;
  IPlanSource& operator=(IPlanSource&&) = delete;

  // The stored plan `id` with its items; fails with not_found when no such plan was stored.
  [[nodiscard]] virtual Result<Plan> load(PlanId id) const = 0;

 protected:
  IPlanSource() = default;
};

}  // namespace myclean::safety
