#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include "core/governor/orphans.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/rules/data_files.hpp"
#include "core/rules/schema.hpp"

// The governor's policy, loaded from data/governor.toml. Every key is optional and defaults to
// the value of the governor spec; the shipped file states the defaults, with comments.
//
//   [governor]
//   mode = "observe"                    # observe: decide and record only; act: apply
//
//   [pressure]                          # entering the pressure state (any one signal)
//   cpu_percent            = 90         # total CPU above this ...
//   cpu_seconds            = 15         # ... for this long
//   available_gib          = 8          # available memory below this (a number: 7.5 is fine)
//   commit_percent         = 90         # commit charge above this share of the commit limit
//   hard_faults_per_second = 1000       # hard faults per second above this ...
//   hard_faults_seconds    = 15         # ... for this long ...
//   hard_faults_below_available_gib = 16  # ... while available memory is below this (with
//                                       # more, hard faults are file reads and page-ins, not
//                                       # a shortage); not below clear.available_gib
//
//   [clear]                             # leaving it: every signal at its exit threshold ...
//   cpu_percent            = 80         # at most
//   available_gib          = 10         # at least
//   commit_percent         = 85         # at most
//   hard_faults_per_second = 500        # at most; default: half of [pressure]'s
//   seconds                = 60         # ... for this long
//
//   [ladder]
//   cooldown_seconds              = 10  # at least this long between two steps on one tree
//   cap_cpu_percent               = 50  # step 3: the cap job's CPU hard cap
//   cap_commit_headroom_percent   = 25  # step 3: commit cap = tree commit + this share ...
//   cap_commit_min_headroom_mib   = 64  # ... but at least this much above it
//   resume_blocks_suspend_minutes = 10  # after the owner's Resume, no new suspend this long
//   cap     = ["build", "test"]         # classes that step 3 caps; other trees stop at EcoQoS
//   suspend = ["build", "test"]         # classes that step 4 suspends; others stop at the cap
//   kill                          = []  # classes whose suspended trees step 5 kills
//                                       # (each list: build, test, default)
//
//   [game_mode]
//   pressure_seconds  = 5               # replaces cpu_seconds and hard_faults_seconds
//   cooldown_seconds  = 5               # replaces ladder.cooldown_seconds
//   build_cpu_percent = 40              # CPU cap of every build job while a game runs
//
//   [jobs]
//   notification_percent = 80           # commit notification limit, share of the commit cap
//   rearm_percent        = 70           # re-armed once the job's commit is below this share
//   [jobs.build]
//   cpu_percent = 75                    # 1..100, CPU hard cap of a build tree job
//   commit_gib  = 96                    # commit cap of a build tree (all its processes)
//   [jobs.test]
//   cpu_percent = 75
//   commit_gib  = 64
//
//   [notifications]
//   interval_minutes = 5                # at most one owner notification per tree this often
//
//   [orphans]                           # orphaned polling loops
//   enabled         = true
//   min_trees       = 3                 # 2..1000 live orphaned trees of one pattern to propose it
//   min_age_minutes = 10                # 1..1440, a younger root is never an orphan loop
//
// Exit thresholds must not be stricter than the entry thresholds (a CPU exit above the entry,
// say), and rearm_percent must be below notification_percent.
namespace myclean::governor {

// Whether the governor applies its decisions. Stored as text: "observe", "act".
enum class GovernorMode {
  // Every decision is recorded in governor_event (detail "observe"); no process, priority or job
  // is changed and no proactive job is created. The pilot's default.
  observe,
  act,  // every decision is applied
};

[[nodiscard]] std::string_view to_string(GovernorMode mode);
[[nodiscard]] std::optional<GovernorMode> parse_governor_mode(std::string_view text);

struct PressurePolicy {
  double cpu_enter_percent = 90.0;
  std::chrono::seconds cpu_dwell{15};
  Bytes available_enter{8ULL << 30U};
  double commit_enter_percent = 90.0;
  double hard_faults_enter = 1000.0;
  std::chrono::seconds hard_faults_dwell{15};
  // Hard faults are a signal only while available memory is below this: with memory to spare
  // they are file reads and page-ins after a shortage, and acting on them only hurts programs.
  Bytes hard_faults_below_available{16ULL << 30U};

  double cpu_exit_percent = 80.0;
  Bytes available_exit{10ULL << 30U};
  double commit_exit_percent = 85.0;
  double hard_faults_exit = 500.0;
  std::chrono::seconds clear_after{60};

  // Game mode's dwell for the CPU and hard-fault signals.
  std::chrono::seconds game_dwell{5};
};

struct LadderPolicy {
  std::chrono::seconds cooldown{10};
  std::chrono::seconds game_cooldown{5};
  int cap_cpu_percent = 50;
  int cap_commit_headroom_percent = 25;
  Bytes cap_commit_min_headroom{64ULL << 20U};
  std::chrono::minutes resume_blocks_suspend{10};
  // Classes whose trees step 3 caps and step 4 suspends. A tree of another class stops at
  // EcoQoS (not in `cap`) or at the cap (not in `suspend`): a program the owner works in is
  // never frozen, and no commit cap makes its allocations fail.
  std::vector<rules::ProcessClass> cap{rules::ProcessClass::build, rules::ProcessClass::test};
  std::vector<rules::ProcessClass> suspend{rules::ProcessClass::build, rules::ProcessClass::test};
  std::vector<rules::ProcessClass> kill;  // empty: step 5 never kills
  std::chrono::minutes notification_interval{5};
};

struct JobPolicy {
  rules::JobCaps build{.cpu_percent = 75, .commit = Bytes{96ULL << 30U}};
  rules::JobCaps test{.cpu_percent = 75, .commit = Bytes{64ULL << 30U}};
  int game_build_cpu_percent = 40;
  int notification_percent = 80;
  int rearm_percent = 70;
};

struct GovernorPolicy {
  GovernorMode mode = GovernorMode::observe;
  PressurePolicy pressure;
  LadderPolicy ladder;
  JobPolicy jobs;
  OrphanPolicy orphans;
};

// Whether step 3 may cap a tree whose root is of `process_class`.
[[nodiscard]] bool cap_enabled(const LadderPolicy& policy, rules::ProcessClass process_class);
// Whether step 4 may suspend a tree whose root is of `process_class`.
[[nodiscard]] bool suspend_enabled(const LadderPolicy& policy, rules::ProcessClass process_class);
// Whether step 5 may kill a suspended tree whose root is of `process_class`.
[[nodiscard]] bool kill_enabled(const LadderPolicy& policy, rules::ProcessClass process_class);

// Validates the whole file: the policy, or one Errc::invalid_data error naming the file and the
// field ("<file>: field 'clear.cpu_percent' must not be above pressure.cpu_percent"), so that
// the caller keeps its previous policy. `file` is used only in messages.
[[nodiscard]] Result<GovernorPolicy> parse_policy_file(std::string_view text,
                                                       const std::filesystem::path& file);

// <data_dir>/governor.toml
[[nodiscard]] Result<GovernorPolicy> load_policy(const rules::IDataFiles& files,
                                                 const std::filesystem::path& data_dir);

}  // namespace myclean::governor
