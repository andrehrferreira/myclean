#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "core/events/connections.hpp"
#include "core/events/events.hpp"
#include "core/events/events_component.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/store/migrations.hpp"
#include "core/telemetry/process_table.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

// The OS sources of the events component; Win32 in the service, fakes in tests.
struct EventSources {
  std::unique_ptr<events::IEventSource> events;
  std::unique_ptr<events::IConnectionTable> connections;
  std::unique_ptr<telemetry::IProcessTable> processes;  // its own: tables are not thread-safe
};

// events::EventsComponent (process and network events, connection snapshots) as a service
// component. ETW being unavailable (for example, the console host without elevation) is listed
// under `unavailable` in `status`; a failed process seed or connection snapshot is a problem.
class EventsAdapter final : public IComponent {
 public:
  // `signers` (may be null) must outlive the component.
  EventsAdapter(EventSources sources, events::ISignerLookup* signers);

  [[nodiscard]] std::string_view name() const override { return events_.name(); }
  [[nodiscard]] std::vector<store::MigrationSet> migrations() const override {
    return events_.migrations();
  }
  [[nodiscard]] Result<void> start(const Context& context) override {
    return events_.start(context.store(), context.clock());
  }
  void stop() override { events_.stop(); }
  [[nodiscard]] ipc::ComponentHealth health() const override;

 private:
  EventSources sources_;
  events::EventsComponent events_;
};

}  // namespace myclean::svc
