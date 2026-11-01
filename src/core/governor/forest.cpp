#include "core/governor/forest.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/rules/schema.hpp"
#include "core/telemetry/process_collector.hpp"

namespace myclean::governor {

namespace {

bool equal_ignoring_case(std::string_view left, std::string_view right) {
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
  return std::ranges::equal(left, right, {}, lower, lower);
}

bool protected_class(rules::ProcessClass process_class) {
  return process_class == rules::ProcessClass::system ||
         process_class == rules::ProcessClass::game || process_class == rules::ProcessClass::allow;
}

// Clears every parent link that points outside the list, at the process itself, or at a
// process not created before it.
void check_parents(std::vector<ProcessNode>& processes) {
  for (ProcessNode& node : processes) {
    if (node.parent && (*node.parent >= processes.size() ||
                        !(processes[*node.parent].key.created < node.key.created))) {
      node.parent.reset();
    }
  }
}

// Per process: a game is among its ancestors. Creation times strictly increase down a link,
// so the walk up always ends.
std::vector<bool> game_descendants(const std::vector<ProcessNode>& processes) {
  std::vector<std::optional<bool>> known(processes.size());
  std::vector<std::size_t> path;
  for (std::size_t start = 0; start < processes.size(); ++start) {
    path.clear();
    std::optional<std::size_t> at = processes[start].parent;
    bool answer = false;
    while (at) {
      if (known[*at]) {
        answer = *known[*at] || processes[*at].process_class == rules::ProcessClass::game;
        break;
      }
      path.push_back(*at);
      at = processes[*at].parent;
    }
    // Resolve the walked ancestors from the top down.
    for (const std::size_t ancestor : std::views::reverse(path)) {
      known[ancestor] = answer;
      answer = answer || processes[ancestor].process_class == rules::ProcessClass::game;
    }
    known[start] = answer;
  }
  std::vector<bool> result;
  result.reserve(known.size());
  for (const std::optional<bool>& answer : known) {
    result.push_back(answer.value_or(false));
  }
  return result;
}

}  // namespace

Forest build_forest(std::vector<ProcessNode> processes, std::optional<std::uint32_t> foreground_pid,
                    std::span<const std::string> allowed_images) {
  Forest forest;
  forest.processes = std::move(processes);
  std::vector<ProcessNode>& nodes = forest.processes;
  check_parents(nodes);

  const std::vector<bool> under_game = game_descendants(nodes);
  forest.protection.resize(nodes.size());
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    const ProcessNode& node = nodes[index];
    forest.protection[index] = protected_class(node.process_class) || under_game[index] ||
                               (foreground_pid && node.key.pid == *foreground_pid) ||
                               std::ranges::any_of(allowed_images, [&](const std::string& image) {
                                 return equal_ignoring_case(image, node.image);
                               });
  }

  std::vector<std::vector<std::size_t>> children(nodes.size());
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    if (nodes[index].parent) {
      children[*nodes[index].parent].push_back(index);
    }
  }
  for (std::size_t root = 0; root < nodes.size(); ++root) {
    if (forest.protection[root] ||
        (nodes[root].parent && !forest.protection[*nodes[root].parent])) {
      continue;
    }
    Tree tree{.root = root};
    tree.members.push_back(root);
    for (std::size_t next = 0; next < tree.members.size(); ++next) {
      for (const std::size_t child : children[tree.members[next]]) {
        if (!forest.protection[child]) {
          tree.members.push_back(child);
        }
      }
    }
    for (const std::size_t member : tree.members) {
      tree.cpu_percent += nodes[member].cpu_percent;
      tree.private_bytes += nodes[member].private_bytes;
    }
    forest.trees.push_back(std::move(tree));
  }
  return forest;
}

std::vector<ProcessNode> nodes_from_view(const telemetry::ProcessView& view) {
  std::vector<ProcessNode> nodes;
  nodes.reserve(view.rows.size());
  for (const telemetry::ProcessRow& row : view.rows) {
    nodes.push_back({.key = {.pid = row.info.pid, .created = row.info.created},
                     .parent = row.parent,
                     .image = row.info.image,
                     .cpu_percent = row.cpu_percent,
                     .private_bytes = row.info.private_bytes});
  }
  return nodes;
}

}  // namespace myclean::governor
