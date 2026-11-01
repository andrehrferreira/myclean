#include "core/governor/pressure.hpp"

#include <array>
#include <chrono>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "core/governor/reason.hpp"
#include "core/model/enum_names.hpp"
#include "core/model/units.hpp"

namespace myclean::governor {

namespace {

constexpr std::array<std::string_view, 2> state_names = {"normal", "pressure"};

// Commit charge as a share of the commit limit, in percent; 0 without a limit.
double commit_percent(const PressureSample& sample) {
  if (sample.commit_limit.count() == 0) {
    return 0.0;
  }
  return 100.0 * static_cast<double>(sample.commit.count()) /
         static_cast<double>(sample.commit_limit.count());
}

// Starts or ends a run of samples that hold a condition.
void track(std::optional<MonoTime>& since, bool holds, MonoTime covered_from) {
  if (!holds) {
    since.reset();
  } else if (!since) {
    since = covered_from;
  }
}

bool held(const std::optional<MonoTime>& since, MonoTime now, std::chrono::seconds duration) {
  return since && now - *since >= duration;
}

}  // namespace

std::string_view to_string(PressureState state) {
  return detail::enum_name(state_names, state);
}

std::optional<PressureState> parse_pressure_state(std::string_view text) {
  return detail::enum_from_name<PressureState>(state_names, text);
}

const PressureStatus& PressureDetector::update(const PressureSample& sample, bool game_mode) {
  const MonoTime covered_from = previous_.value_or(sample.time - std::chrono::seconds{1});
  previous_ = sample.time;
  const double commit = commit_percent(sample);

  track(cpu_since_, sample.cpu_percent > policy_.cpu_enter_percent, covered_from);
  track(faults_since_, sample.hard_faults_per_second > policy_.hard_faults_enter, covered_from);
  const std::chrono::seconds cpu_dwell = game_mode ? policy_.game_dwell : policy_.cpu_dwell;
  const std::chrono::seconds faults_dwell =
      game_mode ? policy_.game_dwell : policy_.hard_faults_dwell;

  std::vector<Reason> reasons;
  if (held(cpu_since_, sample.time, cpu_dwell)) {
    reasons.push_back(Reason::cpu);
  }
  if (sample.available < policy_.available_enter) {
    reasons.push_back(Reason::available_memory);
  }
  if (commit > policy_.commit_enter_percent) {
    reasons.push_back(Reason::commit);
  }
  if (held(faults_since_, sample.time, faults_dwell)) {
    reasons.push_back(Reason::hard_faults);
  }

  if (status_.state == PressureState::normal) {
    if (!reasons.empty()) {
      status_ = {
          .state = PressureState::pressure, .reasons = std::move(reasons), .clearing = false};
      calm_since_.reset();
    }
    return status_;
  }

  if (!reasons.empty()) {
    status_.reasons = std::move(reasons);
  }
  const bool calm = sample.cpu_percent <= policy_.cpu_exit_percent &&
                    sample.available >= policy_.available_exit &&
                    commit <= policy_.commit_exit_percent &&
                    sample.hard_faults_per_second <= policy_.hard_faults_exit;
  track(calm_since_, calm, covered_from);
  if (held(calm_since_, sample.time, policy_.clear_after)) {
    status_ = {};
    calm_since_.reset();
    return status_;
  }
  status_.clearing = calm_since_.has_value();
  return status_;
}

}  // namespace myclean::governor
