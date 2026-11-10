#pragma once

#include <cstddef>
#include <memory>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/process_collector.hpp"

// The read side of guided freeze elimination: the answer of input.pauses (core/ipc/input.hpp).
namespace myclean::elimination {

// The pauses listed in input.pauses, newest first.
inline constexpr std::size_t max_listed_pauses = 50;

// input.pauses at `now`: the baseline (or that it is still being collected), the ranked suspects
// with whether they run now (`view`; null: unknown), the offer, the active pause and the pauses
// with their rates.
[[nodiscard]] Result<nlohmann::json> pauses_answer(
    store::Store& store, const std::shared_ptr<const telemetry::ProcessView>& view, WallTime now);

}  // namespace myclean::elimination
