#include "core/governor/notify.hpp"

#include <iterator>
#include <map>

#include "core/governor/forest.hpp"
#include "core/model/units.hpp"

namespace myclean::governor {

bool NotificationLimiter::allow(const ProcessKey& tree, MonoTime now) {
  const auto found = last_.find(tree);
  if (found != last_.end() && now - found->second < interval_) {
    return false;
  }
  last_[tree] = now;
  return true;
}

void NotificationLimiter::prune(MonoTime now) {
  for (auto entry = last_.begin(); entry != last_.end();) {
    entry = now - entry->second >= interval_ ? last_.erase(entry) : std::next(entry);
  }
}

}  // namespace myclean::governor
