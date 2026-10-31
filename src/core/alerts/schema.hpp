#pragma once

#include "core/store/migrations.hpp"

namespace myclean::alerts {

// The alert engine's tables (component "alerts"), registered with store::Store::open:
//   alert           every raised alert: rule, entity, severity, values, first and last
//                   occurrence, occurrence count and whether it is active; kept 2 years
//   alert_snooze    the snooze of each (rule, entity) until a time
//   alert_baseline  the previous reading of each (rule, entity) of a change rule, so that a
//                   change while the service was stopped is still detected
[[nodiscard]] const store::MigrationSet& alert_migrations();

}  // namespace myclean::alerts
