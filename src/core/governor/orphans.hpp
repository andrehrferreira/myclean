#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/governor/forest.hpp"
#include "core/model/units.hpp"

// Orphaned polling loops (governor spec, "Orphaned Polling Loops"): trees whose root is a shell,
// an interpreter or a polling tool whose launching process is gone, and whose command line
// repeats in several such trees (seen 2026-11-01: 88 watch_channel.sh / collab.py loops left by
// closed sessions). Pure: the process table of one second in, the groups out.
namespace myclean::governor {

struct OrphanPolicy {
  bool enabled = true;
  int min_trees = 3;                 // a pattern is proposed from this many live trees
  std::chrono::minutes min_age{10};  // a root younger than this is never an orphan loop
};

// What the owner decided for a pattern. Stored as text: "always", "ignore".
enum class OrphanRule {
  always,  // end every orphaned tree of the pattern, now and later
  ignore,  // never propose the pattern again
};

[[nodiscard]] std::string_view to_string(OrphanRule rule);
[[nodiscard]] std::optional<OrphanRule> parse_orphan_rule(std::string_view text);

// The owner's answer to a proposal ("End N loops of <pattern>"), or forgetting a stored rule.
// As text: "end_now", "always", "ignore", "forget".
enum class OrphanDecision {
  end_now,  // end the proposal's trees now
  always,   // store the pattern as `always` and end its trees now
  ignore,   // store the pattern as `ignore`
  forget,   // remove a stored rule
};

[[nodiscard]] std::optional<OrphanDecision> parse_orphan_decision(std::string_view text);

// One process of the table, as orphan detection sees it.
struct OrphanProcess {
  ProcessKey key;
  std::uint32_t parent_pid = 0;  // the creator's PID as the OS reports it, which may be gone
  // Index of the parent in the same list when that parent was created before this process (a
  // reused PID is never a parent), as telemetry::ProcessRow::parent gives it.
  std::optional<std::size_t> parent;
  std::string image;
  // Never ended, nor anything below it: class system, game or allow, the foreground window's
  // process, MyClean itself.
  bool protected_process = false;
  // The command line, when known; filled for orphan_roots() before group_orphans().
  std::optional<std::string> command_line;
};

// A shell, interpreter or polling tool, by file name, case ignored: bash, sh, dash, zsh, fish,
// pwsh, powershell, cmd, python (any version, pythonw, py), node, deno, bun, perl, ruby, php,
// tail, sleep, timeout, watch, inotifywait, wscript and cscript.
[[nodiscard]] bool loop_image(std::string_view image);

// The pattern of a command line: the program reduced to its file name without ".exe", quotes
// dropped, white space collapsed, lower case, and every token or path part that is a number, or
// a run of 8 or more hex digits and dashes holding a digit (a PID, a port, a time, a session id),
// replaced by '#'. At most max_pattern_length characters.
inline constexpr std::size_t max_pattern_length = 300;
[[nodiscard]] std::string normalize_command_line(std::string_view command_line);

// The roots of orphaned loop trees at `now`: processes that are not protected, run a loop image,
// were created at least policy.min_age before `now`, and whose creator is gone (no process with
// its parent PID created before it). Empty when the policy is disabled.
[[nodiscard]] std::vector<std::size_t> orphan_roots(std::span<const OrphanProcess> processes,
                                                    WallTime now, const OrphanPolicy& policy);

// A root and its descendants; a protected process and everything below it are left out.
struct OrphanTree {
  std::size_t root = 0;
  std::vector<std::size_t> members;  // the root first
};

// The trees whose roots' command lines share one pattern.
struct OrphanGroup {
  std::string pattern;
  std::string image;  // the first root's
  std::vector<OrphanTree> trees;
};

// Groups `roots` (from orphan_roots) by the pattern of their command line, in order of first
// appearance; a root without a command line is left out, since nothing shows it repeats.
[[nodiscard]] std::vector<OrphanGroup> group_orphans(std::span<const OrphanProcess> processes,
                                                     std::span<const std::size_t> roots);

}  // namespace myclean::governor
