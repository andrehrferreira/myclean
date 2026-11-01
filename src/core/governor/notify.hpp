#pragma once

#include <chrono>
#include <map>

#include "core/governor/forest.hpp"
#include "core/model/units.hpp"

// Owner notifications (governor spec, "Rate-Limited Owner Notifications"): at most one per tree
// per interval (default 5 min). A tree is named by its root process.
namespace myclean::governor {

class NotificationLimiter {
 public:
  explicit NotificationLimiter(std::chrono::minutes interval) : interval_(interval) {}

  void set_interval(std::chrono::minutes interval) { interval_ = interval; }

  // True, and counted as shown, when the tree had no notification in the last interval.
  [[nodiscard]] bool allow(const ProcessKey& tree, MonoTime now);
  // A notification shown regardless of the limit (the suspend notification, which the owner
  // must be able to act on); later ones are limited from `now`.
  void record(const ProcessKey& tree, MonoTime now) { last_[tree] = now; }
  // Drops the trees whose last notification is older than the interval.
  void prune(MonoTime now);

 private:
  std::chrono::minutes interval_;
  std::map<ProcessKey, MonoTime> last_;
};

}  // namespace myclean::governor
