#include "core/governor/recovery.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include "core/governor/jobs.hpp"
#include "core/governor/policy.hpp"
#include "core/governor/reason.hpp"
#include "core/model/enum_names.hpp"

namespace myclean::governor {

namespace {

constexpr std::array<std::string_view, 3> role_names = {"class", "tree", "cap"};
constexpr std::array<std::string_view, 6> recovery_names = {
    "adopt_job", "release_job", "forget_job", "untrusted_job", "resume", "forget_suspend"};

RecoveryCommand job_command(const PersistedJob& job, const std::vector<ReopenedJob>& reopened,
                            const JobPolicy& policy, bool game_mode) {
  const auto found = std::ranges::find(reopened, job.name, &ReopenedJob::name);
  const JobOpen outcome = found == reopened.end() ? JobOpen::missing : found->outcome;
  if (outcome == JobOpen::missing) {
    return {.kind = RecoveryKind::forget_job, .job_name = job.name};
  }
  if (outcome == JobOpen::untrusted) {
    return {
        .kind = RecoveryKind::untrusted_job, .job_name = job.name, .reason = Reason::untrusted_job};
  }
  const std::optional<JobLimits> limits = class_job_limits(job.process_class, policy, game_mode);
  if (found->member_pids.empty() || job.role == JobRole::cap_job || !limits) {
    return {.kind = RecoveryKind::release_job, .job_name = job.name};
  }
  return {.kind = RecoveryKind::adopt_job, .job_name = job.name, .limits = limits};
}

}  // namespace

std::string_view to_string(JobRole role) {
  return detail::enum_name(role_names, role);
}

std::optional<JobRole> parse_job_role(std::string_view text) {
  return detail::enum_from_name<JobRole>(role_names, text);
}

std::string_view to_string(RecoveryKind kind) {
  return detail::enum_name(recovery_names, kind);
}

std::vector<RecoveryCommand> plan_recovery(const RecoveryInput& input, const JobPolicy& policy) {
  std::vector<RecoveryCommand> commands;
  commands.reserve(input.jobs.size() + input.suspends.size());
  for (const PersistedJob& job : input.jobs) {
    commands.push_back(job_command(job, input.reopened, policy, input.game_mode));
  }
  for (const PersistedSuspend& suspend : input.suspends) {
    const bool alive = std::ranges::find(input.processes, suspend.process) != input.processes.end();
    if (!alive || suspend.count <= 0) {
      commands.push_back({.kind = RecoveryKind::forget_suspend, .process = suspend.process});
    } else if (std::ranges::find(input.accounted, suspend.process) == input.accounted.end()) {
      commands.push_back({.kind = RecoveryKind::resume,
                          .process = suspend.process,
                          .times = suspend.count,
                          .reason = Reason::orphaned_suspend});
    }
  }
  return commands;
}

}  // namespace myclean::governor
