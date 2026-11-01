#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/units.hpp"
#include "core/safety/safe_remove.hpp"

namespace myclean::cleaning {

// What the cleaning worker applies now (cleaning.status "applying").
struct Applying {
  enum class Kind { plan, purge };

  Kind kind = Kind::plan;
  // The plan applied, or the quarantine run deleted; nullopt for the scheduled purge.
  std::optional<PlanId> plan;
  std::string title;
  WallTime started_at;
  std::int64_t items_done = 0;
  std::int64_t items_total = 0;
  Bytes bytes_done;  // of the items' stored sizes
  Bytes bytes_total;
  std::int64_t files_done = 0;  // files removed, moved or copied so far
  std::string current;          // the item handled now; empty between items
};

[[nodiscard]] std::string_view to_string(Applying::Kind kind);

// Keeps what the cleaning worker applies now: told by SafeRemove as it goes, on the worker
// thread, and read from any thread. While nothing runs, the IProgress calls are ignored.
class ApplyTracker final : public safety::IProgress {
 public:
  ApplyTracker() = default;

  // A plan or a purge starts; its totals hold until SafeRemove begins on its items.
  void start(Applying applying);
  void finish();
  // What runs now; nullopt when nothing does.
  [[nodiscard]] std::optional<Applying> now() const;

  void begin(std::size_t items, Bytes bytes) override;
  void item_started(const PlanItem& item) override;
  void file_done(Bytes bytes) override;
  void item_finished(const safety::ItemOutcome& outcome) override;

 private:
  mutable std::mutex mutex_;  // guards the members below
  std::optional<Applying> applying_;
  Bytes item_base_;  // bytes_done when the current item started
  Bytes item_size_;  // its stored size
  Bytes item_done_;  // what its files reported so far
};

}  // namespace myclean::cleaning
