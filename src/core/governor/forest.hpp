#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/model/units.hpp"
#include "core/rules/schema.hpp"
#include "core/telemetry/process_collector.hpp"

// The process trees the ladder acts on (governor spec, "Escalation Ladder", "Protected Processes
// Receive No Action", "Tree Membership Tracking").
namespace myclean::governor {

// A process instance: PIDs are reused, a PID with its creation time is not.
struct ProcessKey {
  std::uint32_t pid = 0;
  WallTime created;
  friend auto operator<=>(const ProcessKey&, const ProcessKey&) = default;
};

// One process of the forest, as the service sees it at one second.
struct ProcessNode {
  ProcessKey key;
  // Index of the parent in the same list, as telemetry::ProcessRow::parent gives it: only a
  // parent created before this process (a reused PID is never a parent). build_forest checks
  // the creation times again and ignores a link that fails.
  std::optional<std::size_t> parent;
  std::string image;
  rules::ProcessClass process_class = rules::ProcessClass::normal;  // final_class()
  double cpu_percent = 0.0;  // share of all logical processors
  Bytes private_bytes;       // commit charge
  bool in_myclean_job = false;
};

// A root and its descendants, none of them protected.
struct Tree {
  std::size_t root = 0;              // index in Forest::processes
  std::vector<std::size_t> members;  // the root first
  double cpu_percent = 0.0;          // summed over the members
  Bytes private_bytes;               // summed over the members
};

// Protected processes, which receive no action: class system, game or allow (and the owner's
// Always-allow images, which count as allow), the foreground window's process, and every
// descendant of a game process.
//
// Tree roots: every process that is not protected and whose parent is gone (exited, never
// linked, or failing the creation-time check) or protected. So children of explorer, services or
// a foreground terminal start trees of their own, and a protected process is never inside a
// tree. Every unprotected process belongs to exactly one tree.
struct Forest {
  std::vector<ProcessNode> processes;  // parent links that failed the check are cleared
  std::vector<bool> protection;        // per process: protected
  std::vector<Tree> trees;             // in the order of their roots in `processes`
};

[[nodiscard]] Forest build_forest(std::vector<ProcessNode> processes,
                                  std::optional<std::uint32_t> foreground_pid,
                                  std::span<const std::string> allowed_images);

// The nodes of a telemetry process view: key, parent link, image, CPU and commit charge. The
// caller fills in process_class and in_myclean_job, which the view does not know.
[[nodiscard]] std::vector<ProcessNode> nodes_from_view(const telemetry::ProcessView& view);

}  // namespace myclean::governor
