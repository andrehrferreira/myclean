#pragma once

#include "core/store/migrations.hpp"

namespace myclean::store {

// The store's own tables (component "store"): time series (series, ts_minute, ts_hour), findings
// and plans (finding, plan, plan_item) and scheduler state (schedule, schedule_run). Times are
// Unix seconds.
[[nodiscard]] const MigrationSet& store_migrations();

}  // namespace myclean::store
