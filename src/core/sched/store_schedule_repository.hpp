#pragma once

#include <optional>
#include <string_view>

#include "core/model/result.hpp"
#include "core/sched/schedule_repository.hpp"
#include "core/store/store.hpp"

namespace myclean::sched {

// The scheduler's state in the store's `schedule` and `schedule_run` tables. Every call is one
// synchronous Store::write or Store::read, so a save is committed when it returns.
class StoreScheduleRepository final : public IScheduleRepository {
 public:
  // `store` must outlive the repository.
  explicit StoreScheduleRepository(store::Store& store) : store_(&store) {}

  // Fails with invalid_data when the stored last result is not a RunStatus.
  [[nodiscard]] Result<std::optional<ScheduleState>> load(std::string_view name) override;
  [[nodiscard]] Result<void> save(const ScheduleState& state) override;
  [[nodiscard]] Result<void> append_run(const RunRecord& record) override;

 private:
  store::Store* store_;
};

}  // namespace myclean::sched
