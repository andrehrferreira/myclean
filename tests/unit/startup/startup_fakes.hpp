#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/startup/startup.hpp"

namespace myclean::startup::testing {

inline std::vector<std::uint8_t> enabled_value() {
  return {2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
}

inline std::vector<std::uint8_t> disabled_value() {
  return {3, 0, 0, 0, 0x6C, 0xA5, 0xB6, 0xB6, 0xFE, 0x68, 0xDD, 0x01};
}

// The start sources in memory; writes change the snapshot the next read returns.
class FakeStartupSystem final : public IStartupSystem {
 public:
  Snapshot snapshot;
  int writes = 0;
  // The write with this number (1-based) fails with access_denied; 0: none fails.
  int fail_write = 0;

  [[nodiscard]] Result<Snapshot> read() const override { return snapshot; }

  [[nodiscard]] Result<void> set_approved(Scope scope, ApprovedKey key, std::string_view name,
                                          const ApprovedValue& value) override {
    if (Result<void> allowed = count(); !allowed) {
      return allowed;
    }
    std::erase_if(snapshot.approved, [&](const Approved& approved) {
      return approved.scope == scope && approved.key == key && approved.name == name;
    });
    if (value) {
      snapshot.approved.push_back(
          Approved{.scope = scope, .key = key, .name = std::string(name), .bytes = *value});
    }
    return {};
  }

  [[nodiscard]] Result<void> set_task_enabled(std::string_view path, bool enabled) override {
    if (Result<void> allowed = count(); !allowed) {
      return allowed;
    }
    const auto task = std::ranges::find(snapshot.tasks, path, &Task::path);
    if (task == snapshot.tasks.end()) {
      return fail(Errc::not_found, "no task");
    }
    task->enabled = enabled;
    return {};
  }

  [[nodiscard]] Result<void> set_service_start(std::string_view name,
                                               const ServiceStart& start) override {
    if (Result<void> allowed = count(); !allowed) {
      return allowed;
    }
    const auto service = std::ranges::find(snapshot.services, name, &Service::name);
    if (service == snapshot.services.end()) {
      return fail(Errc::not_found, "no service");
    }
    service->start = start;
    return {};
  }

  [[nodiscard]] const Approved* approved(Scope scope, ApprovedKey key,
                                         std::string_view name) const {
    const auto found = std::ranges::find_if(snapshot.approved, [&](const Approved& approved) {
      return approved.scope == scope && approved.key == key && approved.name == name;
    });
    return found == snapshot.approved.end() ? nullptr : &*found;
  }

 private:
  Result<void> count() {
    ++writes;
    if (writes == fail_write) {
      return fail(Errc::access_denied, "denied");
    }
    return {};
  }
};

}  // namespace myclean::startup::testing
