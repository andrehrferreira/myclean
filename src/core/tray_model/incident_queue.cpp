#include "core/tray_model/incident_queue.hpp"

#include <deque>
#include <iterator>
#include <mutex>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/input/incident.hpp"
#include "core/input/incident_record.hpp"
#include "core/ipc/input.hpp"
#include "core/model/result.hpp"
#include "core/tray_model/service_link.hpp"

namespace myclean::tray_model {

void IncidentQueue::record(const input::Incident& incident) {
  const std::scoped_lock lock(mutex_);
  queue_.push_back(incident);
  trim();
}

int IncidentQueue::forward(IServiceLink& link) {
  std::deque<input::Incident> sending;
  {
    const std::scoped_lock lock(mutex_);
    sending.swap(queue_);
  }
  int stored = 0;
  std::uint64_t rejected = 0;
  while (!sending.empty()) {
    const Result<nlohmann::json> reply = link.call(
        ipc::input_record_incident_method, {{"incident", input::to_record(sending.front())}});
    if (!reply && !link.reachable()) {
      break;
    }
    if (reply) {
      ++stored;
    } else {
      ++rejected;  // the service answered with an error: resending would fail the same way
    }
    sending.pop_front();
  }
  const std::scoped_lock lock(mutex_);
  counts_.sent += static_cast<std::uint64_t>(stored);
  counts_.rejected += rejected;
  // What arrived while sending is newer than what is left.
  queue_.insert(queue_.begin(), std::make_move_iterator(sending.begin()),
                std::make_move_iterator(sending.end()));
  trim();
  return stored;
}

IncidentQueue::Counts IncidentQueue::counts() const {
  const std::scoped_lock lock(mutex_);
  Counts counts = counts_;
  counts.queued = queue_.size();
  return counts;
}

void IncidentQueue::trim() {
  while (queue_.size() > capacity_) {
    queue_.pop_front();
    ++counts_.dropped;
  }
}

}  // namespace myclean::tray_model
