#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <vector>

#include "core/input/incident.hpp"
#include "core/input/probe.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"

namespace myclean::input {

// The copy/paste freeze state machine (spec input-watchdog).
//
// Copy: a Ctrl+C starts a watch that ends at the next clipboard update; the time between them is
// the copy latency. A new Ctrl+C restarts the watch; a watch without update within watch_limit
// is dropped (Ctrl+C that copied nothing).
// Paste: a Ctrl+V starts a watch that probes the foreground window, and the clipboard owner when
// it belongs to another thread, with WM_NULL every probe_period until both answered or
// watch_limit passed. Ctrl+V during a paste watch is ignored.
// During every watch the clipboard's open holder is sampled each round. A watch whose latency,
// owner unanswered time or input lag exceeds incident_threshold becomes an Incident with its
// context (formats, viewer, hung windows, suspects, CPU and memory load), passed to the sink.
//
// Threads: key() and clipboard_updated() come from the input thread and only queue the event
// with its receipt time; poll() runs on the probing thread, so blocking probes never delay a
// receipt time stamp.
class Watchdog {
 public:
  // The arguments must outlive the watchdog.
  Watchdog(IInputProbe& probe, const os::IClock& clock, IIncidentSink& sink);

  // A Ctrl+C or Ctrl+V keydown received at monotonic time `received`; `input_lag` is the tick
  // count at receipt minus the key's message time.
  void key(Chord chord, MonoTime received, std::chrono::milliseconds input_lag);
  // A WM_CLIPBOARDUPDATE received at monotonic time `received`.
  void clipboard_updated(MonoTime received);

  // Applies the queued events, runs the probe round that is due, records the incidents of
  // finished watches, and returns when to call again; nullopt when nothing is watched, in which
  // case the next call is due after the next event.
  [[nodiscard]] std::optional<MonoTime> poll();

 private:
  struct Event {
    std::optional<Chord> chord;  // nullopt: clipboard update
    MonoTime received;
    WallTime time;
    std::chrono::milliseconds input_lag{};
  };

  struct Watch {
    Chord chord = Chord::copy;
    MonoTime start;
    WallTime time;
    std::chrono::milliseconds input_lag{};
    MonoTime next_round;
    bool started = false;
    std::optional<CpuTimes> cpu_start;
    std::optional<WindowRef> foreground;
    std::optional<WindowRef> owner;  // paste: the owner when the watch started
    bool probe_owner = false;
    std::optional<MonoTime> answered;  // copy: clipboard update; paste: foreground's answer
    std::optional<MonoTime> owner_answered;
    std::optional<ProcessRef> holder;
  };

  void apply(const Event& event);
  void round(Watch& watch);
  [[nodiscard]] static bool done(const Watch& watch, MonoTime now);
  void finish(const Watch& watch);

  IInputProbe* probe_;
  const os::IClock* clock_;
  IIncidentSink* sink_;

  std::mutex mutex_;  // guards inbox_
  std::vector<Event> inbox_;

  // Probing thread only.
  std::optional<Watch> copy_;
  std::optional<Watch> paste_;
};

}  // namespace myclean::input
