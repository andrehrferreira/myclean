#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/input/classifier.hpp"
#include "core/input/incident.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

namespace myclean::input {

// The watchdog's table (component "input"), registered with store::Store::open:
//   input_incident  one row per incident; lists (formats, hung, suspects) as JSON text
[[nodiscard]] const store::MigrationSet& input_migrations();

// Keeps incidents for 2 years (Store::history_retention); pass to Store::add_retention.
[[nodiscard]] store::RetentionRule input_retention();

// Stores `incident` synchronously and returns its row id.
[[nodiscard]] Result<std::int64_t> save_incident(store::Store& store, const Incident& incident);

struct StoredIncident {
  std::int64_t id = 0;
  Incident incident;
  Classification classification;
};

// The incidents of one mechanism and one process, for the Freezes view.
struct FreezeGroup {
  Mechanism mechanism = Mechanism::unexplained;
  // Image name of the process the classifier named; none when it named no process.
  std::optional<std::string> process;
  WallTime latest;
  std::vector<StoredIncident> incidents;  // newest first; the count is its size
};

// Every stored incident, classified now and grouped by mechanism and process image; groups are
// ordered by their latest incident, newest first. Empty when no incident was recorded. Fails
// with invalid_data when a row cannot be read back.
[[nodiscard]] Result<std::vector<FreezeGroup>> freeze_summary(store::Store& store);

}  // namespace myclean::input
