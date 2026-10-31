#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/alerts/config.hpp"
#include "core/model/ids.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/store/database.hpp"
#include "core/store/store.hpp"

// The alert engine (alerts spec, PRD RPT-2): evaluates the rules of alerts.toml against the
// readings producers report, keeps the stored alert records, rate-limits toasts per alert,
// snoozes alerts and maintains the active set behind the tray badge. Delivery (toasts, badge)
// belongs to the tray; this module only decides what to deliver.
namespace myclean::alerts {

using AlertId = Id<struct AlertTag>;

// An alert is the state of one rule for one entity ("volume.fill-up" for "C:").
struct AlertKey {
  std::string rule;
  std::string entity;

  friend auto operator<=>(const AlertKey&, const AlertKey&) = default;
};

// One fact from a producer (telemetry, disk health). Readings whose metric no rule watches are
// ignored, as is a NaN value. A volume that is not filling reports an infinite days-until-full.
struct Reading {
  std::string metric;
  std::string entity;
  double value = 0.0;
  std::string detail;  // optional context for the owner, such as "38.2 GiB free"
};

// One stored alert record. Firings of the same key while its record is active, or within the
// rate-limit window after it ended, count as occurrences of that record.
struct Alert {
  AlertId id;
  AlertKey key;
  std::string title;
  Severity severity = Severity::warning;
  double value = 0.0;              // the reading of the latest occurrence
  std::optional<double> limit;     // threshold rules
  std::optional<double> previous;  // change rules: the reading before the change
  std::string unit;
  std::string detail;
  WallTime first;
  WallTime last;
  std::int64_t count = 0;
  bool active = false;
  std::optional<WallTime> snoozed_until;  // filled by active() while the alert is snoozed

  friend bool operator==(const Alert&, const Alert&) = default;
};

// What the tray icon shows.
struct Badge {
  int count = 0;  // active alerts that are not snoozed
  std::optional<Severity> worst;
};

// Thread-safe. Writes are synchronous (store::Store::write) and only happen when something
// changes: an alert fires or ends, a change rule's reading changes, a snooze is set.
//
// A threshold rule fires when a reading of an entity goes beyond the limit after one that was
// not (or the first reading is beyond it); the alert ends at the first reading back within
// the limit. A change rule fires on every qualifying change; its alert stays active until the
// owner dismisses it. A firing produces a toast unless the key is snoozed or had a toast within
// the rate-limit window.
class AlertEngine {
 public:
  // `store` must have been opened with alert_migrations(); it and `clock` must outlive the
  // engine. Loads the active alerts, snoozes and change baselines, and adds the 2-year
  // retention of the `alert` table.
  [[nodiscard]] static Result<std::unique_ptr<AlertEngine>> open(store::Store& store,
                                                                 const os::IClock& clock,
                                                                 AlertConfig config);

  AlertEngine(const AlertEngine&) = delete;
  AlertEngine& operator=(const AlertEngine&) = delete;
  AlertEngine(AlertEngine&&) = delete;
  AlertEngine& operator=(AlertEngine&&) = delete;
  ~AlertEngine() = default;

  // New rules after alerts.toml was reloaded; the state of existing alerts is kept.
  void set_config(AlertConfig config);

  // Evaluates the readings. Returns the alerts to show as toasts now, in firing order. On a
  // store error the readings after the failed write are not evaluated.
  [[nodiscard]] Result<std::vector<Alert>> report(std::span<const Reading> readings);

  // No toasts and no badge for `key` until `duration` from now; occurrences are still
  // recorded. A later snooze replaces an earlier one.
  [[nodiscard]] Result<void> snooze(const AlertKey& key, std::chrono::seconds duration);
  // The owner has seen the alert: it leaves the active set until it fires again.
  [[nodiscard]] Result<void> dismiss(const AlertKey& key);

  // The active alerts, ordered by key, snoozed ones included.
  [[nodiscard]] std::vector<Alert> active() const;
  [[nodiscard]] Badge badge() const;
  // Stored alerts whose last occurrence is at or after `since`, newest first (alerts.list).
  [[nodiscard]] Result<std::vector<Alert>> list(WallTime since) const;

 private:
  struct State {
    std::optional<Alert> alert;      // the latest record of the key
    bool beyond = false;             // threshold rules: the latest reading was beyond the limit
    std::optional<double> baseline;  // change rules: the latest reading
    std::optional<WallTime> last_toast;
  };

  AlertEngine(store::Store& store, const os::IClock& clock, AlertConfig config);

  [[nodiscard]] Result<void> load();
  [[nodiscard]] WallTime now() const;
  [[nodiscard]] bool snoozed(const AlertKey& key, WallTime now) const;
  [[nodiscard]] Result<void> evaluate(const AlertRule& rule, const Reading& reading, WallTime now,
                                      std::vector<Alert>& toasts);
  [[nodiscard]] Result<void> fire(const AlertRule& rule, const Reading& reading,
                                  std::optional<double> previous, WallTime now,
                                  std::vector<Alert>& toasts);
  // Writes `alert` (inserted when it has no id yet) and makes it the key's record.
  [[nodiscard]] Result<void> save(Alert alert);

  store::Store* store_;
  const os::IClock* clock_;
  mutable std::mutex mutex_;  // guards the members below
  AlertConfig config_;
  std::map<AlertKey, State> states_;
  std::map<AlertKey, WallTime> snoozes_;
};

}  // namespace myclean::alerts
