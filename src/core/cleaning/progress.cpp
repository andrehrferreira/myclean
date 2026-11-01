#include "core/cleaning/progress.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

#include "core/model/plan.hpp"
#include "core/model/units.hpp"
#include "core/safety/paths.hpp"
#include "core/safety/safe_remove.hpp"

namespace myclean::cleaning {

std::string_view to_string(Applying::Kind kind) {
  return kind == Applying::Kind::purge ? "purge" : "plan";
}

void ApplyTracker::start(Applying applying) {
  const std::scoped_lock lock(mutex_);
  applying_ = std::move(applying);
  item_base_ = applying_->bytes_done;
  item_size_ = Bytes{};
  item_done_ = Bytes{};
}

void ApplyTracker::finish() {
  const std::scoped_lock lock(mutex_);
  applying_.reset();
}

std::optional<Applying> ApplyTracker::now() const {
  const std::scoped_lock lock(mutex_);
  return applying_;
}

void ApplyTracker::begin(std::size_t items, Bytes bytes) {
  const std::scoped_lock lock(mutex_);
  if (applying_) {
    applying_->items_total = applying_->items_done + static_cast<std::int64_t>(items);
    applying_->bytes_total = applying_->bytes_done + bytes;
  }
}

void ApplyTracker::item_started(const PlanItem& item) {
  const std::scoped_lock lock(mutex_);
  if (applying_) {
    applying_->current = safety::to_utf8(item.path);
    item_base_ = applying_->bytes_done;
    item_size_ = item.size;
    item_done_ = Bytes{};
  }
}

void ApplyTracker::file_done(Bytes bytes) {
  const std::scoped_lock lock(mutex_);
  if (applying_) {
    ++applying_->files_done;
    item_done_ += bytes;
    // An item's files may add up to more than its stored size: the bar never runs ahead of it.
    applying_->bytes_done = item_base_ + std::min(item_done_, item_size_);
  }
}

void ApplyTracker::item_finished(const safety::ItemOutcome& /*outcome*/) {
  const std::scoped_lock lock(mutex_);
  if (applying_) {
    ++applying_->items_done;
    applying_->bytes_done = item_base_ + item_size_;  // skipped or not, the item is done
    applying_->current.clear();
  }
}

}  // namespace myclean::cleaning
