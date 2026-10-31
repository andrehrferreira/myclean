#include "core/sched/schedule_repository.hpp"

#include <array>
#include <optional>
#include <string_view>

#include "core/model/enum_names.hpp"
#include "core/model/result.hpp"

namespace myclean::sched {
namespace {

constexpr std::array<std::string_view, 4> run_status_names{"running", "succeeded", "failed",
                                                           "skipped"};

}  // namespace

std::string_view to_string(RunStatus status) {
  return detail::enum_name(run_status_names, status);
}

std::optional<RunStatus> parse_run_status(std::string_view text) {
  return detail::enum_from_name<RunStatus>(run_status_names, text);
}

Result<std::optional<ScheduleState>> MemoryScheduleRepository::load(std::string_view name) {
  const auto row = rows_.find(name);
  if (row == rows_.end()) {
    return std::optional<ScheduleState>{};
  }
  return row->second;
}

Result<void> MemoryScheduleRepository::save(const ScheduleState& state) {
  rows_.insert_or_assign(state.name, state);
  return {};
}

Result<void> MemoryScheduleRepository::append_run(const RunRecord& record) {
  runs_.push_back(record);
  return {};
}

}  // namespace myclean::sched
