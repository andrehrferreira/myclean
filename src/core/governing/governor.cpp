#include "core/governing/governor.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "core/events/events.hpp"
#include "core/games/detector.hpp"
#include "core/games/fullscreen.hpp"
#include "core/governing/executor.hpp"
#include "core/governing/job_book.hpp"
#include "core/governing/keys.hpp"
#include "core/governing/methods.hpp"
#include "core/governing/records.hpp"
#include "core/governor/classifier.hpp"
#include "core/governor/commands.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/game_mode.hpp"
#include "core/governor/jobs.hpp"
#include "core/governor/ladder.hpp"
#include "core/governor/orphans.hpp"
#include "core/governor/policy.hpp"
#include "core/governor/pressure.hpp"
#include "core/governor/reason.hpp"
#include "core/governor/recovery.hpp"
#include "core/ipc/governor.hpp"
#include "core/model/actor.hpp"
#include "core/model/result.hpp"
#include "core/model/text.hpp"
#include "core/model/units.hpp"
#include "core/os/jobs.hpp"
#include "core/os/process_control.hpp"
#include "core/rules/schema.hpp"
#include "core/store/database.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/telemetry/process_collector.hpp"

namespace myclean::governing {

namespace {

using Json = nlohmann::json;
namespace metric = telemetry::metric;

// Parent links are followed at most this far (creation times make them acyclic anyway).
constexpr int max_ancestors = 64;

std::string utf8(const std::filesystem::path& path) {
  return utf8_text(path);
}

std::filesystem::path from_utf8(std::string_view text) {
  return {std::u8string(text.begin(), text.end())};
}

bool equal_ignoring_case(std::string_view left, std::string_view right) {
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
  return std::ranges::equal(left, right, {}, lower, lower);
}

bool protected_class(rules::ProcessClass process_class) {
  return process_class == rules::ProcessClass::system ||
         process_class == rules::ProcessClass::game || process_class == rules::ProcessClass::allow;
}

// MyClean's own processes are never ended, whatever their class.
bool myclean_image(std::string_view image) {
  return image.size() >= 7 && equal_ignoring_case(image.substr(0, 7), "myclean");
}

std::vector<std::vector<governor::ProcessKey>> tree_keys(
    const std::vector<governor::OrphanProcess>& table, const governor::OrphanGroup& group) {
  std::vector<std::vector<governor::ProcessKey>> trees;
  trees.reserve(group.trees.size());
  for (const governor::OrphanTree& tree : group.trees) {
    std::vector<governor::ProcessKey> members;
    members.reserve(tree.members.size());
    for (const std::size_t member : tree.members) {
      members.push_back(table[member].key);
    }
    trees.push_back(std::move(members));
  }
  return trees;
}

bool job_class(rules::ProcessClass process_class) {
  return process_class == rules::ProcessClass::build || process_class == rules::ProcessClass::test;
}

std::string limits_text(const governor::JobLimits& limits) {
  std::string text = std::format("CPU {} %, commit {} GiB", limits.cpu_percent,
                                 static_cast<double>(limits.commit.count()) / (1ULL << 30U));
  if (limits.notification) {
    text += std::format(", notification at {} GiB",
                        static_cast<double>(limits.notification->count()) / (1ULL << 30U));
  }
  return text;
}

}  // namespace

Governor::Governor(GovernorSources sources, store::Store& store, const os::IClock& clock)
    : sources_(std::move(sources)),
      store_(&store),
      clock_(&clock),
      recorder_(store, clock),
      jobs_(*sources_.jobs, store, recorder_),
      executor_(*sources_.processes, jobs_, store, recorder_, clock),
      pressure_(policy_.pressure),
      ladder_(policy_.ladder),
      detector_(games::GameSources{}, games::FullscreenGpuPolicy{}, clock) {}

Result<void> Governor::start() {
  const std::scoped_lock lock(mutex_);
  Result<std::vector<std::string>> allowed = load_allowed(*store_);
  if (!allowed) {
    return tl::unexpected(allowed.error());
  }
  allowed_ = std::move(*allowed);
  Result<std::vector<OrphanRuleRecord>> rules = load_orphan_rules(*store_);
  if (!rules) {
    return tl::unexpected(rules.error());
  }
  orphan_rules_ = std::move(*rules);
  reload();
  game_mode_ = governor::decide_game_mode(override_, false, policy_.jobs);
  scan_games();
  recover();
  started_ = true;
  spdlog::info("governor: {} mode, {} class entries, {} allowed images",
               governor::to_string(policy_.mode), classifier_.classes().matches.size(),
               allowed_.size());
  return {};
}

void Governor::reload() {
  reloaded_at_ = clock_->monotonic();
  if (sources_.policy) {
    if (Result<governor::GovernorPolicy> policy = sources_.policy(); policy) {
      apply_policy(*policy);
      problems_.erase("policy");
    } else {
      problems_["policy"] = "governor.toml kept the previous policy: " + policy.error().message;
    }
  }
  if (sources_.classes) {
    if (Result<void> reloaded = classifier_.reload(sources_.classes()); reloaded) {
      problems_.erase("classes");
      for (auto& [key, known] : known_) {
        known.process_class = classifier_.classify(known.facts).process_class;
      }
    } else {
      problems_["classes"] = "the previous classes stay in effect: " + reloaded.error().message;
    }
  }
}

void Governor::apply_policy(const governor::GovernorPolicy& policy) {
  if (started_ && policy.mode != policy_.mode) {
    spdlog::info("governor: mode {} -> {}", governor::to_string(policy_.mode),
                 governor::to_string(policy.mode));
    fail_open(governor::Reason::policy,
              std::format("mode changed to {}", governor::to_string(policy.mode)));
  }
  const bool caps_changed =
      policy.jobs.build.cpu_percent != policy_.jobs.build.cpu_percent ||
      policy.jobs.build.commit != policy_.jobs.build.commit ||
      policy.jobs.test.cpu_percent != policy_.jobs.test.cpu_percent ||
      policy.jobs.test.commit != policy_.jobs.test.commit ||
      policy.jobs.notification_percent != policy_.jobs.notification_percent ||
      policy.jobs.game_build_cpu_percent != policy_.jobs.game_build_cpu_percent;
  policy_ = policy;
  pressure_.set_policy(policy_.pressure);
  ladder_.set_policy(policy_.ladder);
  jobs_.set_rearm_percent(policy_.jobs.rearm_percent);
  executor_.set_mode(policy_.mode);
  if (started_ && caps_changed) {
    // The same state with the reloaded caps: under `auto`, active stays active.
    game_mode_ = governor::decide_game_mode(override_, game_mode_.active, policy_.jobs);
    const std::size_t changed = apply_class_limits();
    if (changed > 0) {
      record(EventKind::job_limits, std::nullopt, {}, std::nullopt, governor::Reason::policy,
             std::format("{} jobs set to the reloaded class caps", changed), true);
    }
  }
}

void Governor::scan_games() {
  rescan_games_ = false;
  if (!sources_.games) {
    return;
  }
  const GameScan scan = sources_.games();
  detector_.update(scan.sources);
  for (auto& [key, known] : known_) {
    if (!known.facts.image_path.empty()) {
      known.game = detector_.classify(from_utf8(known.facts.image_path));
    }
  }
  if (scan.problems.empty()) {
    problems_.erase("games");
  } else {
    std::string text = "game detection: ";
    for (const std::string& problem : scan.problems) {
      text += (text.ends_with(": ") ? "" : "; ") + problem;
    }
    problems_["games"] = std::move(text);
  }
  spdlog::info("governor: {} game library roots, {} GameConfigStore entries",
               detector_.roots().roots().size(), detector_.game_config().paths().size());
}

void Governor::recover() {
  Result<std::vector<JobRecord>> records = load_jobs(*store_);
  Result<std::vector<governor::PersistedSuspend>> suspends = load_suspends(*store_);
  if (!records || !suspends) {
    problems_["recovery"] = "persisted jobs or suspends unreadable: " +
                            (records ? suspends.error().message : records.error().message);
    return;
  }
  governor::RecoveryInput input{.suspends = *suspends};
  std::map<std::string, std::unique_ptr<os::IJob>> opened;
  std::map<std::string, JobRecord> by_name;
  for (const JobRecord& record : *records) {
    const std::string name = record.ref.object_name();
    by_name.emplace(name, record);
    input.jobs.push_back(governor::PersistedJob{
        .name = name, .process_class = record.process_class, .role = record.role});
    Result<std::unique_ptr<os::IJob>> job = sources_.jobs->open(record.ref);
    if (job) {
      const Result<std::vector<std::uint32_t>> members = (*job)->members();
      input.reopened.push_back(
          governor::ReopenedJob{.name = name,
                                .outcome = governor::JobOpen::opened,
                                .member_pids = members ? *members : std::vector<std::uint32_t>{}});
      opened.emplace(name, std::move(*job));
    } else if (job.error().code == Errc::refused) {
      input.reopened.push_back(
          governor::ReopenedJob{.name = name, .outcome = governor::JobOpen::untrusted});
    } else if (job.error().code != Errc::not_found) {
      spdlog::warn("governor: re-open {}: {}", name, job.error().message);
    }
  }
  // A recorded suspend counts as alive here: resume() checks the creation time itself, so a
  // dead or reused PID is refused rather than resumed.
  for (const governor::PersistedSuspend& suspend : *suspends) {
    input.processes.push_back(suspend.process);
  }
  const bool act = policy_.mode == governor::GovernorMode::act;
  for (const governor::RecoveryCommand& command : governor::plan_recovery(input, policy_.jobs)) {
    const auto record_of = by_name.find(command.job_name);
    switch (command.kind) {
      case governor::RecoveryKind::adopt_job: {
        std::unique_ptr<os::IJob>& job = opened.at(command.job_name);
        if (!act) {
          static_cast<void>(job->lift_all());
          static_cast<void>(job->release());
          static_cast<void>(forget_job(*store_, record_of->second.ref));
          record(EventKind::job_released, record_of->second.root, record_of->second.root_image,
                 record_of->second.process_class, governor::Reason::policy,
                 "job of the previous run " + command.job_name + " lifted (observe mode)", true);
          break;
        }
        const governor::JobLimits limits = command.limits.value_or(governor::JobLimits{});
        Result<TrackedJob*> adopted = jobs_.adopt(std::move(job), record_of->second, limits);
        record(EventKind::job_adopted, record_of->second.root, record_of->second.root_image,
               record_of->second.process_class, governor::Reason::policy,
               adopted ? "job of the previous run " + command.job_name +
                             " adopted: " + limits_text(limits)
                       : "adopting " + command.job_name + " failed: " + adopted.error().message,
               true);
        break;
      }
      case governor::RecoveryKind::release_job: {
        const std::unique_ptr<os::IJob>& job = opened.at(command.job_name);
        static_cast<void>(job->lift_all());
        static_cast<void>(job->release());
        static_cast<void>(forget_job(*store_, record_of->second.ref));
        record(EventKind::job_released, record_of->second.root, record_of->second.root_image,
               record_of->second.process_class, governor::Reason::policy,
               "job of the previous run " + command.job_name + " lifted and released", true);
        break;
      }
      case governor::RecoveryKind::forget_job:
        static_cast<void>(forget_job(*store_, record_of->second.ref));
        break;
      case governor::RecoveryKind::untrusted_job:
        static_cast<void>(forget_job(*store_, record_of->second.ref));
        record(EventKind::untrusted_job, record_of->second.root, record_of->second.root_image,
               record_of->second.process_class, governor::Reason::untrusted_job,
               command.job_name + " is owned by someone else: not adopted", false);
        break;
      case governor::RecoveryKind::resume:
        executor_.resume_persisted(
            governor::PersistedSuspend{.process = command.process, .count = command.times},
            command.reason);
        break;
      case governor::RecoveryKind::forget_suspend:
        static_cast<void>(forget_suspend(*store_, command.process));
        break;
    }
  }
}

void Governor::stop() {
  const std::scoped_lock lock(mutex_);
  if (stopped_) {
    return;
  }
  stopped_ = true;
  fail_open(governor::Reason::stop, "service stopping");
  spdlog::info("governor: stopped; every cap lifted and every suspend resumed");
}

void Governor::fail_open(governor::Reason reason, std::string_view detail) {
  for (const governor::Command& command : ladder_.stop()) {
    run(command);
  }
  jobs_.release_all(reason, detail);
  if (Result<std::vector<governor::PersistedSuspend>> suspends = load_suspends(*store_); suspends) {
    for (const governor::PersistedSuspend& suspend : *suspends) {
      executor_.resume_persisted(suspend, reason);
    }
  } else {
    spdlog::error("governor: persisted suspends unreadable: {}", suspends.error().message);
  }
  executor_.clear();
  notifications_.clear();
  observed_roots_.clear();
  orphan_proposals_.clear();
}

void Governor::run(const governor::Command& command) {
  const bool act = policy_.mode == governor::GovernorMode::act;
  switch (command.kind) {
    case governor::CommandKind::notify: {
      if (act) {
        std::erase_if(notifications_, [&](const Notification& shown) {
          return shown.command.root == command.root;
        });
        notifications_.push_back(
            Notification{.id = next_notification_++, .time = clock_->now(), .command = command});
        while (notifications_.size() > ipc::max_notifications) {
          notifications_.pop_front();
        }
      }
      record(EventKind::notify, command.root, command.root_image, command.process_class,
             command.reason, act ? "shown to the owner" : "observe", false);
      return;
    }
    case governor::CommandKind::add_to_allow_list: {
      std::string detail = "added to the owner's allow list";
      if (!allowed(command.root_image)) {
        if (Result<void> added = add_allowed(*store_, command.root_image, clock_->now()); added) {
          allowed_.push_back(command.root_image);
        } else {
          detail = "not stored: " + added.error().message;
        }
      }
      GovernorEvent event{.kind = EventKind::add_to_allow_list,
                          .root = command.root,
                          .root_image = command.root_image,
                          .process_class = command.process_class,
                          .reason = command.reason,
                          .actor = command.actor,
                          .detail = std::move(detail)};
      recorder_.record(std::move(event), true);
      return;
    }
    default:
      executor_.execute(command);
      return;
  }
}

Governor::Known Governor::learn(governor::ProcessFacts facts,
                                std::optional<governor::ProcessKey> parent) const {
  Known known{.facts = std::move(facts), .parent = parent};
  known.process_class = classifier_.classify(known.facts).process_class;
  if (!known.facts.image_path.empty()) {
    known.game = detector_.classify(from_utf8(known.facts.image_path));
  }
  return known;
}

void Governor::on_start(const events::ProcessStart& start) {
  const std::scoped_lock lock(mutex_);
  if (stopped_ || !started_) {
    return;
  }
  const governor::ProcessKey key = to_governor(start.process);
  const std::filesystem::path path = from_utf8(start.image);
  std::optional<governor::ProcessKey> parent;
  if (start.parent) {
    parent = to_governor(*start.parent);
  }
  Known learned = learn(governor::ProcessFacts{.image = utf8(path.filename()),
                                               .image_path = start.image,
                                               .command_line = start.command_line.value_or(""),
                                               .signer = start.signer},
                        parent);
  const Known& known = known_.insert_or_assign(key, std::move(learned)).first->second;
  const rules::ProcessClass process_class = class_of(key);
  if (!job_class(process_class)) {
    return;
  }
  const std::optional<UserState> user = fresh_user_state();
  if ((user && user->foreground_pid == key.pid) || under_class(key, rules::ProcessClass::game)) {
    return;  // protected: the foreground window's process, or a game's descendant
  }
  const bool act = policy_.mode == governor::GovernorMode::act;
  jobs_.drain();
  const bool parent_in_job =
      parent && (act ? jobs_.in_tree_job(*parent) : under_observed_root(*parent));
  const Result<governor::JobMembership> membership = jobs_.membership(key);
  if (!membership) {
    return;  // exited already
  }
  const governor::JobPlacement placement =
      governor::place_job(process_class, parent_in_job, *membership);
  if (placement == governor::JobPlacement::none) {
    return;
  }
  if (!act) {
    observed_roots_.insert(key);
    record(EventKind::place_job, key, known.facts.image, process_class, governor::Reason::policy,
           std::format("observe: {}", governor::to_string(placement)), false);
    return;
  }
  place(key, known, process_class, placement);
}

void Governor::place(const governor::ProcessKey& key, const Known& known,
                     rules::ProcessClass process_class, governor::JobPlacement placement) {
  const std::optional<governor::JobLimits> caps =
      governor::class_job_limits(process_class, policy_.jobs, game_mode_.active);
  if (!caps) {
    return;
  }
  const governor::JobLimits& limits = *caps;
  const auto failed = [&](const std::string& why) {
    record(EventKind::place_job, key, known.facts.image, process_class, governor::Reason::policy,
           "not placed: " + why, false);
  };
  TrackedJob* parent = nullptr;
  if (placement == governor::JobPlacement::class_job_then_tree_job) {
    parent = jobs_.class_job(process_class);
    if (parent == nullptr) {
      Result<TrackedJob*> created = jobs_.create(governor::JobRole::class_job, process_class,
                                                 std::nullopt, {}, limits, clock_->now());
      if (!created) {
        failed("class job not created: " + created.error().message);
        return;
      }
      parent = *created;
    }
  }
  Result<TrackedJob*> tree = jobs_.create(governor::JobRole::tree_job, process_class, key,
                                          known.facts.image, limits, clock_->now());
  if (!tree) {
    failed("tree job not created: " + tree.error().message);
    return;
  }
  const std::string name = (*tree)->job->ref().object_name();
  if (Result<void> assigned = os::assign_tree_root(
          to_os(key), parent != nullptr ? parent->job.get() : nullptr, *(*tree)->job);
      !assigned) {
    jobs_.release(**tree, governor::Reason::policy, "its root could not be assigned");
    failed("assignment to " + name + " failed: " + assigned.error().message);
    return;
  }
  (*tree)->members.insert(key);
  std::string detail = std::format("tree job {} ({})", name, limits_text(limits));
  if (parent != nullptr) {
    parent->members.insert(key);
    detail += ", nested under class job " + parent->job->ref().object_name();
  } else {
    detail += ", nested under the job it was in";
  }
  const Result<os::PriorityClass> priority =
      sources_.processes->set_priority(to_os(key), os::PriorityClass::below_normal);
  detail +=
      priority ? ", priority below normal" : ", priority not lowered: " + priority.error().message;
  record(EventKind::place_job, key, known.facts.image, process_class, governor::Reason::policy,
         std::move(detail), true);
}

void Governor::on_stop(const events::ProcessStop& stop) {
  const std::scoped_lock lock(mutex_);
  if (stopped_) {
    return;
  }
  detector_.fullscreen().forget(stop.process.pid);
  const governor::ProcessKey key = to_governor(stop.process);
  known_.erase(key);
  observed_roots_.erase(key);
}

void Governor::tick() {
  const std::scoped_lock lock(mutex_);
  if (stopped_ || !started_) {
    return;
  }
  if (!reloaded_at_ || clock_->monotonic() - *reloaded_at_ >= reload_interval) {
    reload();
  }
  if (rescan_games_) {
    scan_games();
  }
  if (paused_until_ && !ladder_.paused(clock_->monotonic())) {
    paused_until_.reset();
  }
  const std::shared_ptr<const telemetry::ProcessView> view =
      sources_.view ? sources_.view() : nullptr;
  std::map<std::uint32_t, WallTime> live;
  if (view) {
    for (const telemetry::ProcessRow& row : view->rows) {
      live.emplace(row.info.pid, row.info.created);
    }
    update_known(*view);
  }
  jobs_.poll(live);
  observe_fullscreen();
  update_game_mode();
  if (view && (!orphans_at_ || clock_->monotonic() - *orphans_at_ >= orphan_interval)) {
    scan_orphans(*view);
  }
  if (view && view->time != last_view_) {
    if (const std::optional<governor::PressureSample> sample = read_sample(); sample) {
      last_view_ = view->time;
      govern(*view, *sample);
    }
  }
  std::erase_if(notifications_, [&](const Notification& notification) {
    const auto found = live.find(notification.command.root.pid);
    return view && (found == live.end() || found->second != notification.command.root.created);
  });
}

void Governor::update_known(const telemetry::ProcessView& view) {
  std::set<governor::ProcessKey> present;
  for (const telemetry::ProcessRow& row : view.rows) {
    const governor::ProcessKey key{.pid = row.info.pid, .created = row.info.created};
    present.insert(key);
    if (const auto found = known_.find(key); found != known_.end()) {
      found->second.session = row.info.session;
      continue;
    }
    std::optional<governor::ProcessKey> parent;
    if (row.parent && *row.parent < view.rows.size()) {
      const telemetry::ProcessInfo& info = view.rows[*row.parent].info;
      parent = governor::ProcessKey{.pid = info.pid, .created = info.created};
    }
    std::optional<std::filesystem::path> path;
    if (sources_.image_path) {
      path = sources_.image_path(key.pid, key.created);
    }
    known_
        .emplace(key, learn(governor::ProcessFacts{.image = row.info.image,
                                                   .image_path = path ? utf8(*path) : ""},
                            parent))
        .first->second.session = row.info.session;
  }
  for (auto entry = known_.begin(); entry != known_.end();) {
    if (!present.contains(entry->first) && entry->first.created + table_grace < view.time) {
      detector_.fullscreen().forget(entry->first.pid);
      observed_roots_.erase(entry->first);
      entry = known_.erase(entry);
    } else {
      ++entry;
    }
  }
}

std::optional<governor::PressureSample> Governor::read_sample() const {
  if (!sources_.metric) {
    return std::nullopt;
  }
  const std::optional<double> cpu = sources_.metric(metric::cpu_utility);
  const std::optional<double> available = sources_.metric(metric::memory_available);
  const std::optional<double> commit = sources_.metric(metric::memory_commit);
  const std::optional<double> limit = sources_.metric(metric::memory_commit_limit);
  if (!cpu || !available || !commit || !limit) {
    return std::nullopt;
  }
  const auto bytes = [](double value) {
    return Bytes{static_cast<std::uint64_t>(std::max(value, 0.0))};
  };
  return governor::PressureSample{
      .time = clock_->monotonic(),
      .cpu_percent = *cpu,
      .available = bytes(*available),
      .commit = bytes(*commit),
      .commit_limit = bytes(*limit),
      .hard_faults_per_second = sources_.metric(metric::memory_hard_faults).value_or(0.0)};
}

void Governor::govern(const telemetry::ProcessView& view, const governor::PressureSample& sample) {
  sample_ = sample;
  recorder_.set_metrics(sample);
  const governor::PressureStatus status = pressure_.update(sample, game_mode_.active);
  std::vector<governor::ProcessNode> nodes = governor::nodes_from_view(view);
  for (governor::ProcessNode& node : nodes) {
    node.process_class = class_of(node.key);
    node.in_myclean_job = jobs_.in_tree_job(node.key);
  }
  const std::optional<UserState> user = fresh_user_state();
  std::optional<std::uint32_t> foreground;
  if (user && user->foreground_pid != 0) {
    foreground = user->foreground_pid;
  }
  for (const governor::Command& command :
       ladder_.tick(governor::LadderInput{.now = sample.time,
                                          .pressure = status,
                                          .metrics = sample,
                                          .processes = std::move(nodes),
                                          .foreground_pid = foreground,
                                          .game_mode = game_mode_.active,
                                          .allowed_images = allowed_})) {
    run(command);
  }
}

void Governor::observe_fullscreen() {
  const UserState user = fresh_user_state().value_or(UserState{});
  games::FullscreenGpuDetector& fullscreen = detector_.fullscreen();
  std::optional<double> gpu_percent;
  const std::optional<std::uint32_t> candidate = fullscreen.candidate();
  if (candidate && sources_.gpu != nullptr) {
    if (!gpu_) {
      if (Result<games::Gpu3dSampler> opened = games::Gpu3dSampler::open(*sources_.gpu); opened) {
        gpu_.emplace(std::move(*opened));
        problems_.erase("gpu");
      } else {
        problems_["gpu"] = "GPU engine counters: " + opened.error().message;
      }
    }
    if (gpu_) {
      if (Result<std::map<std::uint32_t, double>> used = gpu_->sample(); used) {
        if (const auto found = used->find(*candidate); found != used->end()) {
          gpu_percent = found->second;
        }
      }
    }
  } else {
    gpu_.reset();
  }
  const std::optional<std::uint32_t> detected = fullscreen.observe(games::FullscreenObservation{
      .state = user.state, .foreground_pid = user.foreground_pid, .gpu_3d_percent = gpu_percent});
  if (!detected) {
    return;
  }
  const auto found =
      std::ranges::find_if(known_, [&](const auto& entry) { return entry.first.pid == *detected; });
  if (found != known_.end()) {
    record(EventKind::game_detected, found->first, found->second.facts.image,
           rules::ProcessClass::game, governor::Reason::policy,
           "fullscreen foreground with sustained GPU use", false);
  }
}

void Governor::update_game_mode() {
  const bool running = std::ranges::any_of(known_, [&](const auto& entry) {
    return class_of(entry.first) == rules::ProcessClass::game;
  });
  const governor::GameMode next = governor::decide_game_mode(override_, running, policy_.jobs);
  if (next.active == game_mode_.active && next.build_cpu_percent == game_mode_.build_cpu_percent) {
    return;
  }
  game_mode_ = next;
  const bool act = policy_.mode == governor::GovernorMode::act;
  const std::size_t changed = act ? apply_class_limits() : 0;
  record(EventKind::game_mode, std::nullopt, {}, std::nullopt, governor::Reason::policy,
         std::format("game mode {} (override {}): build CPU cap {} %{}", next.active ? "on" : "off",
                     governor::to_string(override_), next.build_cpu_percent,
                     act ? std::format(" on {} jobs", changed) : std::string(", observe")),
         act && changed > 0);
}

std::size_t Governor::apply_class_limits() {
  std::size_t changed = 0;
  for (const std::unique_ptr<TrackedJob>& tracked : jobs_.jobs()) {
    TrackedJob& job = *tracked;
    if (job.role == governor::JobRole::cap_job) {
      continue;
    }
    const std::optional<governor::JobLimits> limits =
        governor::class_job_limits(job.process_class, policy_.jobs, game_mode_.active);
    if (!limits) {
      continue;
    }
    governor::JobLimits target = *limits;
    if (job.tightened_from) {
      job.tightened_from = *limits;
      target = job.limits;
      target.cpu_percent = std::min(job.limits.cpu_percent, limits->cpu_percent);
    }
    if (target == job.limits) {
      continue;
    }
    if (Result<void> set = JobBook::set_limits(job, target); !set) {
      spdlog::warn("governor: limits of {}: {}", job.job->ref().object_name(), set.error().message);
      continue;
    }
    if (!job.notification.armed()) {
      static_cast<void>(job.job->clear_commit_notification());
    }
    ++changed;
  }
  return changed;
}

std::vector<governor::OrphanProcess> Governor::orphan_table(
    const telemetry::ProcessView& view) const {
  const std::optional<UserState> user = fresh_user_state();
  std::vector<governor::OrphanProcess> table;
  table.reserve(view.rows.size());
  for (const telemetry::ProcessRow& row : view.rows) {
    const governor::ProcessKey key{.pid = row.info.pid, .created = row.info.created};
    table.push_back(governor::OrphanProcess{
        .key = key,
        .parent_pid = row.info.parent_pid,
        .parent = row.parent,
        .image = row.info.image,
        .protected_process = protected_class(class_of(key)) || myclean_image(row.info.image) ||
                             (user && user->foreground_pid == key.pid)});
  }
  return table;
}

void Governor::read_command_lines(std::vector<governor::OrphanProcess>& table,
                                  const std::vector<std::size_t>& roots) {
  for (const std::size_t root : roots) {
    governor::OrphanProcess& process = table[root];
    const auto found = known_.find(process.key);
    if (found == known_.end()) {
      continue;
    }
    Known& known = found->second;
    if (known.facts.command_line.empty() && !known.command_line_read && sources_.command_line) {
      known.command_line_read = true;
      if (std::optional<std::string> line =
              sources_.command_line(process.key.pid, process.key.created);
          line && !line->empty()) {
        known.facts.command_line = std::move(*line);
        known.process_class = classifier_.classify(known.facts).process_class;
        process.protected_process =
            process.protected_process || protected_class(class_of(process.key));
      }
    }
    if (!known.facts.command_line.empty()) {
      process.command_line = known.facts.command_line;
    }
  }
}

void Governor::scan_orphans(const telemetry::ProcessView& view) {
  orphans_at_ = clock_->monotonic();
  std::vector<governor::OrphanProcess> table = orphan_table(view);
  const std::vector<std::size_t> roots = governor::orphan_roots(table, view.time, policy_.orphans);
  read_command_lines(table, roots);
  std::set<governor::ProcessKey> live;
  for (const std::size_t root : roots) {
    live.insert(table[root].key);
  }
  std::erase_if(orphans_ended_,
                [&](const governor::ProcessKey& key) { return !live.contains(key); });
  std::vector<OrphanProposal> proposals;
  for (const governor::OrphanGroup& group : governor::group_orphans(table, roots)) {
    std::vector<std::vector<governor::ProcessKey>> trees = tree_keys(table, group);
    const auto rule = std::ranges::find(orphan_rules_, group.pattern, &OrphanRuleRecord::pattern);
    if (rule == orphan_rules_.end()) {
      if (std::cmp_greater_equal(trees.size(), policy_.orphans.min_trees)) {
        propose(group, std::move(trees), proposals);
      }
    } else if (rule->rule == governor::OrphanRule::always) {
      end_always(group, trees);
    }
  }
  orphan_proposals_ = std::move(proposals);
}

void Governor::propose(const governor::OrphanGroup& group,
                       std::vector<std::vector<governor::ProcessKey>> trees,
                       std::vector<OrphanProposal>& proposals) {
  const auto known = std::ranges::find(orphan_proposals_, group.pattern, &OrphanProposal::pattern);
  if (known != orphan_proposals_.end()) {
    OrphanProposal kept = *known;
    kept.trees = std::move(trees);
    proposals.push_back(std::move(kept));
    return;
  }
  record(EventKind::orphan_loops, std::nullopt, group.image, std::nullopt,
         governor::Reason::orphaned_loop,
         std::format("proposed to the owner: end {} orphaned trees of '{}'", trees.size(),
                     group.pattern),
         false);
  proposals.push_back(OrphanProposal{.id = next_orphan_proposal_++,
                                     .time = clock_->now(),
                                     .pattern = group.pattern,
                                     .image = group.image,
                                     .trees = std::move(trees)});
}

void Governor::end_always(const governor::OrphanGroup& group,
                          const std::vector<std::vector<governor::ProcessKey>>& trees) {
  std::size_t ended = 0;
  for (const std::vector<governor::ProcessKey>& members : trees) {
    if (orphans_ended_.insert(members.front()).second) {
      end_orphan_tree(members, Actor::governor());
      ++ended;
    }
  }
  if (ended > 0) {
    const bool act = policy_.mode == governor::GovernorMode::act;
    record(EventKind::orphan_loops, std::nullopt, group.image, std::nullopt,
           governor::Reason::orphaned_loop,
           std::format("{} {} orphaned trees of '{}' (the owner's rule: always)",
                       act ? "ended" : "observe: would end", ended, group.pattern),
           act);
  }
}

void Governor::end_orphan_tree(const std::vector<governor::ProcessKey>& members,
                               const Actor& actor) {
  const governor::ProcessKey& root = members.front();
  const auto known = known_.find(root);
  run(governor::Command{
      .kind = governor::CommandKind::kill,
      .actor = actor,
      .root = root,
      .root_image = known == known_.end() ? std::string() : known->second.facts.image,
      .process_class = class_of(root),
      .members = members,
      .step = governor::Step::kill,
      .reason = governor::Reason::orphaned_loop,
      .metrics = sample_.value_or(governor::PressureSample{})});
}

Result<void> Governor::store_orphan_rule(const OrphanProposal& proposal,
                                         governor::OrphanRule rule) {
  Result<std::int64_t> id =
      save_orphan_rule(*store_, proposal.pattern, proposal.image, rule, clock_->now());
  if (!id) {
    return tl::unexpected(id.error());
  }
  std::erase_if(orphan_rules_,
                [&](const OrphanRuleRecord& stored) { return stored.pattern == proposal.pattern; });
  orphan_rules_.push_back(OrphanRuleRecord{.id = *id,
                                           .pattern = proposal.pattern,
                                           .image = proposal.image,
                                           .rule = rule,
                                           .decided = clock_->now()});
  recorder_.record(
      GovernorEvent{.kind = EventKind::orphan_rule,
                    .root_image = proposal.image,
                    .reason = governor::Reason::orphaned_loop,
                    .actor = Actor::owner(),
                    .detail = std::format(
                        "{} '{}'", rule == governor::OrphanRule::always ? "always end" : "ignore",
                        proposal.pattern)},
      true);
  return {};
}

Result<std::size_t> Governor::decide_orphans(std::int64_t id, governor::OrphanDecision decision) {
  const std::scoped_lock lock(mutex_);
  if (stopped_ || !started_) {
    return fail(Errc::invalid_state, "the governor is not running");
  }
  if (decision == governor::OrphanDecision::forget) {
    const auto rule = std::ranges::find(orphan_rules_, id, &OrphanRuleRecord::id);
    if (rule == orphan_rules_.end()) {
      return fail(Errc::not_found, std::format("no orphaned-loop rule {}", id));
    }
    if (Result<void> forgotten = forget_orphan_rule(*store_, id); !forgotten) {
      return tl::unexpected(forgotten.error());
    }
    recorder_.record(
        GovernorEvent{.kind = EventKind::orphan_rule,
                      .root_image = rule->image,
                      .reason = governor::Reason::orphaned_loop,
                      .actor = Actor::owner(),
                      .detail = std::format("forgot the rule '{}' for '{}'",
                                            governor::to_string(rule->rule), rule->pattern)},
        true);
    orphan_rules_.erase(rule);
    return std::size_t{0};
  }
  // The trees as they are now: one may have exited, or its loop may no longer be orphaned.
  if (const std::shared_ptr<const telemetry::ProcessView> view =
          sources_.view ? sources_.view() : nullptr) {
    update_known(*view);
    scan_orphans(*view);
  }
  const auto found = std::ranges::find(orphan_proposals_, id, &OrphanProposal::id);
  if (found == orphan_proposals_.end()) {
    return fail(Errc::not_found,
                std::format("no orphaned-loop proposal {}: its loops ended or are no longer "
                            "orphaned",
                            id));
  }
  if (decision != governor::OrphanDecision::end_now) {
    const governor::OrphanRule rule = decision == governor::OrphanDecision::always
                                          ? governor::OrphanRule::always
                                          : governor::OrphanRule::ignore;
    if (Result<void> stored = store_orphan_rule(*found, rule); !stored) {
      return tl::unexpected(stored.error());
    }
  }
  std::size_t ended = 0;
  if (decision != governor::OrphanDecision::ignore) {
    for (const std::vector<governor::ProcessKey>& members : found->trees) {
      orphans_ended_.insert(members.front());
      end_orphan_tree(members, Actor::owner());
      ++ended;
    }
  }
  orphan_proposals_.erase(found);
  return ended;
}

Json Governor::orphans_json() const {
  Json proposals = Json::array();
  for (const OrphanProposal& proposal : orphan_proposals_) {
    std::size_t processes = 0;
    for (const std::vector<governor::ProcessKey>& members : proposal.trees) {
      processes += members.size();
    }
    proposals.push_back(Json{{"id", proposal.id},
                             {"time", store::to_unix_seconds(proposal.time)},
                             {"pattern", proposal.pattern},
                             {"image", proposal.image},
                             {"trees", proposal.trees.size()},
                             {"processes", processes}});
  }
  Json rules = Json::array();
  for (const OrphanRuleRecord& rule : orphan_rules_) {
    rules.push_back(Json{{"id", rule.id},
                         {"pattern", rule.pattern},
                         {"image", rule.image},
                         {"rule", governor::to_string(rule.rule)},
                         {"time", store::to_unix_seconds(rule.decided)}});
  }
  return Json{{"enabled", policy_.orphans.enabled},
              {"min_trees", policy_.orphans.min_trees},
              {"min_age_minutes", policy_.orphans.min_age.count()},
              {"proposals", std::move(proposals)},
              {"rules", std::move(rules)}};
}

bool Governor::protects(const governor::ProcessKey& key) const {
  const std::scoped_lock lock(mutex_);
  if (!started_ || stopped_) {
    return true;
  }
  const auto found = known_.find(key);
  const std::optional<UserState> user = fresh_user_state();
  if (found == known_.end() || !user || user->foreground_pid == key.pid) {
    return true;
  }
  return protected_class(class_of(key)) || myclean_image(found->second.facts.image) ||
         under_class(key, rules::ProcessClass::game);
}

rules::ProcessClass Governor::class_of(const governor::ProcessKey& key) const {
  const auto found = known_.find(key);
  if (found == known_.end()) {
    return rules::ProcessClass::normal;
  }
  const Known& known = found->second;
  // A service (session 0) is the system's: drivers' helpers, Hyper-V, vendor services. The
  // governor acts on the owner's session only.
  if (known.session == 0U) {
    return rules::ProcessClass::system;
  }
  rules::ProcessClass process_class = known.process_class;
  if (process_class != rules::ProcessClass::system && allowed(known.facts.image)) {
    process_class = rules::ProcessClass::allow;
  }
  return governor::final_class(process_class,
                               known.game.has_value() || detector_.fullscreen().detected(key.pid));
}

bool Governor::allowed(std::string_view image) const {
  return std::ranges::any_of(
      allowed_, [&](const std::string& entry) { return equal_ignoring_case(entry, image); });
}

bool Governor::under_class(const governor::ProcessKey& key,
                           rules::ProcessClass process_class) const {
  std::optional<governor::ProcessKey> at = key;
  for (int depth = 0; depth < max_ancestors && at; ++depth) {
    const auto found = known_.find(*at);
    if (found == known_.end()) {
      return false;
    }
    at = found->second.parent;
    if (at && class_of(*at) == process_class) {
      return true;
    }
  }
  return false;
}

bool Governor::under_observed_root(const governor::ProcessKey& key) const {
  std::optional<governor::ProcessKey> at = key;
  for (int depth = 0; depth < max_ancestors && at; ++depth) {
    if (observed_roots_.contains(*at)) {
      return true;
    }
    const auto found = known_.find(*at);
    at = found == known_.end() ? std::nullopt : found->second.parent;
  }
  return false;
}

std::optional<UserState> Governor::fresh_user_state() const {
  if (!user_state_ || !user_state_at_ ||
      clock_->monotonic() - *user_state_at_ > std::chrono::seconds{ipc::user_state_seconds}) {
    return std::nullopt;
  }
  return user_state_;
}

std::size_t Governor::choose(const governor::ProcessKey& root, governor::OwnerChoice choice) {
  const std::scoped_lock lock(mutex_);
  if (stopped_ || !started_) {
    return 0;
  }
  const std::vector<governor::Command> commands = ladder_.choose(root, choice, clock_->monotonic());
  for (const governor::Command& command : commands) {
    run(command);
  }
  std::erase_if(notifications_, [&](const Notification& notification) {
    return notification.command.root == root;
  });
  return commands.size();
}

std::optional<WallTime> Governor::pause(std::chrono::minutes minutes) {
  const std::scoped_lock lock(mutex_);
  if (stopped_ || !started_) {
    return std::nullopt;
  }
  const MonoTime now = clock_->monotonic();
  if (minutes.count() <= 0) {
    if (ladder_.paused(now)) {
      for (const governor::Command& command : ladder_.pause(now, std::chrono::minutes{0})) {
        run(command);
      }
    }
    paused_until_.reset();
    return std::nullopt;
  }
  for (const governor::Command& command : ladder_.pause(now, minutes)) {
    run(command);
  }
  notifications_.clear();
  paused_until_ = clock_->now() + minutes;
  return paused_until_;
}

governor::GameMode Governor::set_override(governor::GameModeOverride value) {
  const std::scoped_lock lock(mutex_);
  override_ = value;
  if (started_ && !stopped_) {
    update_game_mode();
  }
  return game_mode_;
}

void Governor::set_user_state(const UserState& state) {
  const std::scoped_lock lock(mutex_);
  const MonoTime now = clock_->monotonic();
  if (!user_state_at_ || now - *user_state_at_ > std::chrono::seconds{ipc::owner_absent_seconds}) {
    rescan_games_ = true;  // the owner logged on: the hive and launchers are readable now
  }
  user_state_ = state;
  user_state_at_ = now;
}

std::vector<std::string> Governor::problems() const {
  const std::scoped_lock lock(mutex_);
  std::vector<std::string> problems;
  problems.reserve(problems_.size() + 1);
  for (const auto& [source, problem] : problems_) {
    problems.push_back(problem);
  }
  if (recorder_.problem()) {
    problems.push_back(*recorder_.problem());
  }
  return problems;
}

void Governor::record(EventKind kind, const std::optional<governor::ProcessKey>& root,
                      std::string_view image, std::optional<rules::ProcessClass> process_class,
                      governor::Reason reason, std::string detail, bool audit) {
  recorder_.record(GovernorEvent{.kind = kind,
                                 .root = root,
                                 .root_image = std::string(image),
                                 .process_class = process_class,
                                 .reason = reason,
                                 .members = root ? std::size_t{1} : std::size_t{0},
                                 .detail = std::move(detail)},
                   audit);
}

Json Governor::status() const {
  const std::scoped_lock lock(mutex_);
  const governor::PressureStatus& pressure = pressure_.status();
  Json reasons = Json::array();
  for (const governor::Reason reason : pressure.reasons) {
    reasons.push_back(governor::to_string(reason));
  }
  Json notifications = Json::array();
  for (const Notification& notification : notifications_) {
    const governor::Command& command = notification.command;
    notifications.push_back(Json{{"id", notification.id},
                                 {"time", store::to_unix_seconds(notification.time)},
                                 {"root", process_json(command.root)},
                                 {"image", command.root_image},
                                 {"class", rules::to_string(command.process_class)},
                                 {"step", governor::to_string(command.step)},
                                 {"reason", governor::to_string(command.reason)},
                                 {"members", command.members.size()}});
  }
  Json events = Json::array();
  for (const GovernorEvent& event : recorder_.recent(ipc::status_events)) {
    events.push_back(event_json(event));
  }
  return Json{{"mode", governor::to_string(policy_.mode)},
              {"paused_until",
               paused_until_ ? Json(store::to_unix_seconds(*paused_until_)) : Json(nullptr)},
              {"pressure", Json{{"state", governor::to_string(pressure.state)},
                                {"clearing", pressure.clearing},
                                {"reasons", std::move(reasons)},
                                {"metrics", metrics_json(sample_)}}},
              {"game_mode", Json{{"active", game_mode_.active},
                                 {"override", governor::to_string(override_)},
                                 {"build_cpu_percent", game_mode_.build_cpu_percent},
                                 {"games", games_json()}}},
              {"jobs", jobs_json()},
              {"trees", trees_json()},
              {"notifications", std::move(notifications)},
              {"orphans", orphans_json()},
              {"events", std::move(events)}};
}

Json Governor::games_json() const {
  Json games = Json::array();
  for (const auto& [key, known] : known_) {
    if (class_of(key) != rules::ProcessClass::game) {
      continue;
    }
    const games::GameReason reason =
        known.game ? known.game->reason : games::GameReason::fullscreen_gpu;
    games.push_back(Json{{"pid", key.pid},
                         {"created", to_nanoseconds(key.created)},
                         {"image", known.facts.image},
                         {"reason", games::to_string(reason)}});
  }
  return games;
}

Json Governor::jobs_json() const {
  Json jobs = Json::array();
  for (const std::unique_ptr<TrackedJob>& job : jobs_.jobs()) {
    const std::optional<Bytes> used = job->committed;
    jobs.push_back(Json{{"name", job->job->ref().object_name()},
                        {"role", governor::to_string(job->role)},
                        {"class", rules::to_string(job->process_class)},
                        {"root", job->root ? process_json(*job->root) : Json(nullptr)},
                        {"root_image", job->root_image},
                        {"members", job->members.size()},
                        {"adopted", job->adopted},
                        {"cpu_cap_percent", job->limits.cpu_percent},
                        {"commit_cap", job->limits.commit.count()},
                        {"commit_used", used ? Json(used->count()) : Json(nullptr)}});
  }
  return jobs;
}

Json Governor::trees_json() const {
  Json trees = Json::array();
  const governor::Forest& forest = ladder_.forest();
  for (const governor::Tree& tree : forest.trees) {
    const governor::ProcessNode& root = forest.processes[tree.root];
    if (const std::optional<governor::Step> step = ladder_.step_of(root.key); step) {
      trees.push_back(Json{{"root", process_json(root.key)},
                           {"image", root.image},
                           {"class", rules::to_string(root.process_class)},
                           {"step", governor::to_string(*step)}});
    }
  }
  return trees;
}

}  // namespace myclean::governing
