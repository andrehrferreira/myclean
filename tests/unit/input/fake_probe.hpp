#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/input/incident.hpp"
#include "core/input/probe.hpp"
#include "core/model/units.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::input::testing {

// 2026-10-31 12:00:00 UTC.
inline const WallTime noon =
    std::chrono::sys_days{std::chrono::October / 31 / 2026} + std::chrono::hours{12};

inline WindowRef window(std::uint64_t handle, std::uint32_t thread, std::uint32_t pid,
                        std::string image) {
  return {.handle = handle, .thread = thread, .process = {.pid = pid, .image = std::move(image)}};
}

// What the fake window manager and clipboard report; the fake also writes `probed`.
struct ProbeScript {
  std::optional<WindowRef> foreground_window;
  std::optional<WindowRef> owner_window;
  std::optional<WindowRef> open_window;
  std::optional<WindowRef> viewer_window;
  // A window answers WM_NULL from this time on; immediately when absent.
  std::map<std::uint64_t, MonoTime> answers_from;
  std::vector<ClipboardFormat> clipboard_formats;
  std::vector<ProcessRef> hung;
  std::vector<std::string> running;
  std::deque<CpuTimes> cpu_readings;  // one per cpu_times() call; nullopt when used up
  std::optional<int> memory_load = 40;
  std::vector<std::uint64_t> probed;  // handles passed to answers(), in order
};

// A probe that reports its script. A probe that would wait longer than its timeout moves the
// clock by the timeout and fails, like SendMessageTimeout; otherwise it moves the clock to the
// answer.
class FakeProbe final : public IInputProbe {
 public:
  FakeProbe(fakes::FakeClock& clock, ProbeScript& script) : clock_(&clock), script_(&script) {}

  std::optional<WindowRef> foreground() override { return script_->foreground_window; }
  std::optional<WindowRef> clipboard_owner() override { return script_->owner_window; }
  std::optional<WindowRef> open_clipboard_window() override { return script_->open_window; }
  std::optional<WindowRef> clipboard_viewer() override { return script_->viewer_window; }
  bool answers(const WindowRef& target, std::chrono::milliseconds timeout) override {
    script_->probed.push_back(target.handle);
    const auto from = script_->answers_from.find(target.handle);
    if (from == script_->answers_from.end() || from->second <= clock_->monotonic()) {
      return true;
    }
    const auto wait = from->second - clock_->monotonic();
    if (wait < timeout) {
      clock_->advance(wait);
      return true;
    }
    clock_->advance(timeout);
    return false;
  }
  std::vector<ClipboardFormat> formats() override { return script_->clipboard_formats; }
  std::vector<ProcessRef> hung_windows() override { return script_->hung; }
  std::vector<std::string> running_images() override { return script_->running; }
  std::optional<CpuTimes> cpu_times() override {
    if (script_->cpu_readings.empty()) {
      return std::nullopt;
    }
    const CpuTimes reading = script_->cpu_readings.front();
    script_->cpu_readings.pop_front();
    return reading;
  }
  std::optional<int> memory_load_percent() override { return script_->memory_load; }

 private:
  fakes::FakeClock* clock_;
  ProbeScript* script_;
};

class RecordingSink final : public IIncidentSink {
 public:
  RecordingSink() = default;

  void record(const Incident& incident) override { incidents.push_back(incident); }

  std::vector<Incident> incidents;
};

}  // namespace myclean::input::testing
