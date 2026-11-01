#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/governor/forest.hpp"
#include "core/model/units.hpp"
#include "core/rules/schema.hpp"

// Builders of process forests for the governor's tests.
namespace myclean::governor::testing {

// Creation times count seconds from the epoch, so a test can order processes by creation.
inline WallTime at(int second) {
  return WallTime{} + std::chrono::seconds{second};
}

inline ProcessKey key(std::uint32_t pid, int created) {
  return {.pid = pid, .created = at(created)};
}

// A process list where each process names its parent by PID; links are resolved like
// telemetry::ProcessRow::parent (to the process with that PID, whatever its creation time:
// build_forest itself must reject a parent created later).
class ForestBuilder {
 public:
  ForestBuilder& add(std::uint32_t pid, std::optional<std::uint32_t> parent_pid, std::string image,
                     rules::ProcessClass process_class = rules::ProcessClass::normal,
                     double cpu_percent = 0.0, Bytes private_bytes = Bytes{}, int created = -1) {
    nodes_.push_back({.key = {.pid = pid, .created = at(created < 0 ? next_created_++ : created)},
                      .image = std::move(image),
                      .process_class = process_class,
                      .cpu_percent = cpu_percent,
                      .private_bytes = private_bytes});
    parents_.push_back(parent_pid);
    return *this;
  }

  [[nodiscard]] std::vector<ProcessNode> build() const {
    std::vector<ProcessNode> nodes = nodes_;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      if (!parents_[index]) {
        continue;
      }
      for (std::size_t other = 0; other < nodes.size(); ++other) {
        if (nodes[other].key.pid == *parents_[index]) {
          nodes[index].parent = other;
        }
      }
    }
    return nodes;
  }

  [[nodiscard]] ProcessKey key_of(std::uint32_t pid) const {
    for (const ProcessNode& node : nodes_) {
      if (node.key.pid == pid) {
        return node.key;
      }
    }
    throw std::logic_error("no process with that PID");
  }

  // Removes the process with `pid` (it exited).
  ForestBuilder& remove(std::uint32_t pid) {
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
      if (nodes_[index].key.pid == pid) {
        nodes_.erase(nodes_.begin() + static_cast<std::ptrdiff_t>(index));
        parents_.erase(parents_.begin() + static_cast<std::ptrdiff_t>(index));
        return *this;
      }
    }
    throw std::logic_error("no process with that PID");
  }

  ProcessNode& node(std::uint32_t pid) {
    for (ProcessNode& node : nodes_) {
      if (node.key.pid == pid) {
        return node;
      }
    }
    throw std::logic_error("no process with that PID");
  }

 private:
  std::vector<ProcessNode> nodes_;
  std::vector<std::optional<std::uint32_t>> parents_;
  int next_created_ = 100;
};

}  // namespace myclean::governor::testing
