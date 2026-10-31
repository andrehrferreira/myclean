#include "svc/events_adapter.hpp"

#include <string>
#include <utility>

#include "core/events/events.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"

namespace myclean::svc {

EventsAdapter::EventsAdapter(EventSources sources, events::ISignerLookup* signers)
    : sources_(std::move(sources)),
      events_(*sources_.events, *sources_.connections, *sources_.processes, signers) {}

ipc::ComponentHealth EventsAdapter::health() const {
  ipc::ComponentHealth health{.name = std::string(name())};
  for (const Error& error : events_.unavailable()) {
    if (error.message.starts_with("ETW unavailable: ")) {
      health.unavailable.push_back(error.message);
    } else {
      health.problems.push_back(error.message);
    }
  }
  health.healthy = health.problems.empty();
  return health;
}

}  // namespace myclean::svc
