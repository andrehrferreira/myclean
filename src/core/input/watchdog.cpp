#include "core/input/watchdog.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/input/incident.hpp"
#include "core/input/probe.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"

namespace myclean::input {

namespace {

std::chrono::milliseconds since(MonoTime start, MonoTime end) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
}

std::optional<ProcessRef> process_of(const std::optional<WindowRef>& window) {
  if (!window) {
    return std::nullopt;
  }
  return window->process;
}

}  // namespace

Watchdog::Watchdog(IInputProbe& probe, const os::IClock& clock, IIncidentSink& sink)
    : probe_(&probe), clock_(&clock), sink_(&sink) {}

void Watchdog::key(Chord chord, MonoTime received, std::chrono::milliseconds input_lag) {
  const Event event{
      .chord = chord, .received = received, .time = clock_->now(), .input_lag = input_lag};
  const std::scoped_lock lock(mutex_);
  inbox_.push_back(event);
}

void Watchdog::clipboard_updated(MonoTime received) {
  const Event event{.chord = std::nullopt, .received = received, .time = clock_->now()};
  const std::scoped_lock lock(mutex_);
  inbox_.push_back(event);
}

std::optional<MonoTime> Watchdog::poll() {
  std::vector<Event> events;
  {
    const std::scoped_lock lock(mutex_);
    events.swap(inbox_);
  }
  for (const Event& event : events) {
    apply(event);
  }

  for (std::optional<Watch>* slot : {&copy_, &paste_}) {
    if (*slot && !done(**slot, clock_->monotonic()) && (*slot)->next_round <= clock_->monotonic()) {
      round(**slot);
    }
  }

  std::optional<MonoTime> next;
  for (std::optional<Watch>* slot : {&copy_, &paste_}) {
    if (!*slot) {
      continue;
    }
    if (done(**slot, clock_->monotonic())) {
      finish(**slot);
      slot->reset();
      continue;
    }
    next = next ? std::min(*next, (*slot)->next_round) : (*slot)->next_round;
  }
  return next;
}

void Watchdog::apply(const Event& event) {
  const auto start = [&](Chord chord) {
    return Watch{.chord = chord,
                 .start = event.received,
                 .time = event.time,
                 .input_lag = event.input_lag,
                 .next_round = event.received};
  };
  if (!event.chord) {
    if (copy_ && !copy_->answered && event.received >= copy_->start) {
      copy_->answered = event.received;
    }
  } else if (*event.chord == Chord::copy) {
    if (copy_ && copy_->answered) {
      finish(*copy_);  // updated, but not yet finished when the next Ctrl+C came
    }
    copy_ = start(Chord::copy);
  } else if (!paste_) {
    paste_ = start(Chord::paste);
  }
}

void Watchdog::round(Watch& watch) {
  watch.next_round = clock_->monotonic() + probe_period;
  if (!watch.started) {
    watch.started = true;
    watch.cpu_start = probe_->cpu_times();
    watch.foreground = probe_->foreground();
    if (watch.chord == Chord::paste) {
      watch.owner = probe_->clipboard_owner();
      watch.probe_owner =
          watch.owner && (!watch.foreground || watch.owner->thread != watch.foreground->thread);
      if (!watch.foreground) {
        watch.answered = watch.start;  // nothing to stall
      }
    }
  }

  const std::optional<WindowRef> holder = probe_->open_clipboard_window();
  if (holder && !watch.holder &&
      (!watch.foreground || holder->process.pid != watch.foreground->process.pid)) {
    watch.holder = holder->process;
  }

  if (watch.chord != Chord::paste) {
    return;
  }
  if (!watch.answered && probe_->answers(*watch.foreground, probe_timeout)) {
    watch.answered = clock_->monotonic();
  }
  if (watch.probe_owner && !watch.owner_answered && probe_->answers(*watch.owner, probe_timeout)) {
    watch.owner_answered = clock_->monotonic();
  }
}

bool Watchdog::done(const Watch& watch, MonoTime now) {
  if (now - watch.start >= watch_limit) {
    return true;
  }
  if (watch.chord == Chord::copy) {
    return watch.answered.has_value();
  }
  return watch.answered && (!watch.probe_owner || watch.owner_answered);
}

void Watchdog::finish(const Watch& watch) {
  if (watch.chord == Chord::copy && !watch.answered) {
    return;  // no clipboard update: the Ctrl+C copied nothing
  }
  Incident incident{.chord = watch.chord,
                    .time = watch.time,
                    .latency = watch.answered ? since(watch.start, *watch.answered) : watch_limit,
                    .capped = !watch.answered,
                    .input_lag = watch.input_lag};
  if (watch.probe_owner) {
    incident.owner_unanswered =
        watch.owner_answered ? since(watch.start, *watch.owner_answered) : watch_limit;
  }
  const bool over =
      incident.latency > incident_threshold ||
      incident.owner_unanswered.value_or(std::chrono::milliseconds{0}) > incident_threshold ||
      incident.input_lag > incident_threshold;
  if (!over) {
    return;
  }

  // A watch that ended before its first round (the probing thread was busy) has no foreground.
  incident.foreground = process_of(watch.started ? watch.foreground : probe_->foreground());
  // Copy: the owner after the update is the copier. Paste: the owner whose data is pasted.
  incident.owner = process_of(watch.chord == Chord::copy ? probe_->clipboard_owner() : watch.owner);
  incident.holder = watch.holder;
  incident.viewer = process_of(probe_->clipboard_viewer());
  incident.formats = describe_formats(probe_->formats());
  incident.hung = probe_->hung_windows();
  const std::vector<std::string> running = probe_->running_images();
  incident.suspects = running_suspects(running);
  const std::optional<CpuTimes> cpu_end = probe_->cpu_times();
  if (watch.cpu_start && cpu_end) {
    incident.cpu_percent = cpu_percent(*watch.cpu_start, *cpu_end);
  }
  incident.memory_load_percent = probe_->memory_load_percent();
  sink_->record(incident);
}

}  // namespace myclean::input
