#include "core/governor/jobs.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "core/governor/policy.hpp"
#include "core/model/enum_names.hpp"
#include "core/model/units.hpp"
#include "core/rules/schema.hpp"

namespace myclean::governor {

namespace {

constexpr std::array<std::string_view, 3> placement_names = {"none", "class_job_then_tree_job",
                                                             "tree_job"};

// `percent` of `bytes`, rounded down.
Bytes share(Bytes bytes, int percent) {
  return Bytes{(bytes.count() / 100U * static_cast<std::uint64_t>(percent)) +
               (bytes.count() % 100U * static_cast<std::uint64_t>(percent) / 100U)};
}

}  // namespace

std::optional<JobLimits> class_job_limits(rules::ProcessClass process_class,
                                          const JobPolicy& policy, bool game_mode) {
  if (process_class != rules::ProcessClass::build && process_class != rules::ProcessClass::test) {
    return std::nullopt;
  }
  const rules::JobCaps& caps =
      process_class == rules::ProcessClass::build ? policy.build : policy.test;
  int cpu_percent = caps.cpu_percent;
  if (game_mode && process_class == rules::ProcessClass::build) {
    cpu_percent = std::min(cpu_percent, policy.game_build_cpu_percent);
  }
  return JobLimits{.cpu_percent = cpu_percent,
                   .commit = caps.commit,
                   .notification = share(caps.commit, policy.notification_percent)};
}

JobLimits cap_job_limits(Bytes tree_commit, const LadderPolicy& policy) {
  const Bytes headroom = std::max(share(tree_commit, policy.cap_commit_headroom_percent),
                                  policy.cap_commit_min_headroom);
  return {.cpu_percent = policy.cap_cpu_percent, .commit = tree_commit + headroom};
}

std::string_view to_string(JobPlacement placement) {
  return detail::enum_name(placement_names, placement);
}

JobPlacement place_job(rules::ProcessClass process_class, bool parent_in_myclean_job,
                       JobMembership membership) {
  if ((process_class != rules::ProcessClass::build && process_class != rules::ProcessClass::test) ||
      parent_in_myclean_job || membership == JobMembership::myclean) {
    return JobPlacement::none;
  }
  return membership == JobMembership::none ? JobPlacement::class_job_then_tree_job
                                           : JobPlacement::tree_job;
}

CommitNotificationLimit::Action CommitNotificationLimit::on_limit_message() {
  if (!armed_) {
    return Action::none;
  }
  armed_ = false;
  return Action::record_and_clear;
}

CommitNotificationLimit::Action CommitNotificationLimit::on_sample(Bytes commit, Bytes cap) {
  if (armed_ || !(commit < share(cap, rearm_percent_))) {
    return Action::none;
  }
  armed_ = true;
  return Action::rearm;
}

}  // namespace myclean::governor
