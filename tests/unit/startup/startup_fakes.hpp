#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/startup/diagnostics.hpp"
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

// An event 101 as the Diagnostics-Performance log renders it: program `name` slowed the boot by
// `degradation_ms`; written at `created` (xs:dateTime).
inline std::string delay_event_xml(std::string_view name, std::uint32_t degradation_ms,
                                   std::string_view created) {
  return "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System>"
         "<Provider Name='Microsoft-Windows-Diagnostics-Performance'/><EventID>101</EventID>"
         "<TimeCreated SystemTime='" +
         std::string(created) + "'/></System><EventData><Data Name='Name'>" + std::string(name) +
         "</Data><Data Name='TotalTime'>" + std::to_string(degradation_ms + 1000) +
         "</Data><Data Name='DegradationTime'>" + std::to_string(degradation_ms) +
         "</Data></EventData></Event>";
}

// The Diagnostics-Performance log in memory: `xml`, newest first, or `error`.
class FakeDiagnosticsLog final : public IDiagnosticsLog {
 public:
  std::vector<std::string> xml;
  std::optional<Error> error;

  [[nodiscard]] Result<std::vector<std::string>> events() const override {
    if (error) {
      return tl::unexpected(*error);
    }
    return xml;
  }
};

}  // namespace myclean::startup::testing
