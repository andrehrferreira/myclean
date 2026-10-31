#include "core/input/watchdog.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "core/input/classifier.hpp"
#include "core/input/incident.hpp"
#include "core/model/units.hpp"
#include "fake_probe.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::input {
namespace {

using namespace std::chrono_literals;
using testing::FakeProbe;
using testing::RecordingSink;
using testing::window;

constexpr std::uint64_t editor = 0x100;  // foreground window, thread 10, pid 1000
constexpr std::uint64_t source = 0x200;  // clipboard owner in another process

class WatchdogTest : public ::testing::Test {
 protected:
  WatchdogTest() {
    script_.foreground_window = window(editor, 10, 1000, "Code.exe");
    script_.owner_window = window(editor, 10, 1000, "Code.exe");
  }

  [[nodiscard]] MonoTime now() const { return clock_.monotonic(); }

  void key(Chord chord, std::chrono::milliseconds lag = 0ms) { watchdog().key(chord, now(), lag); }

  // Polls, moving the clock to each requested time, until the watchdog is idle or `until`.
  // Returns the number of polls.
  int run(std::optional<MonoTime> until = std::nullopt) {
    int polls = 0;
    for (;;) {
      const std::optional<MonoTime> next = watchdog().poll();
      ++polls;
      if (!next || polls > 1000) {
        return polls;
      }
      if (until && *next > *until) {
        clock().advance(*until - now());
        return polls;
      }
      if (*next > now()) {
        clock().advance(*next - now());
      }
    }
  }

  // Polls until `elapsed` has passed since the call.
  void run_for(std::chrono::milliseconds elapsed) { run(now() + elapsed); }

  void owner_in_other_process(std::chrono::milliseconds busy_for) {
    script().owner_window = window(source, 20, 2000, "Blender.exe");
    script().answers_from[source] = now() + busy_for;
  }

  fakes::FakeClock& clock() { return clock_; }
  testing::ProbeScript& script() { return script_; }
  RecordingSink& sink() { return sink_; }
  Watchdog& watchdog() { return watchdog_; }

 private:
  fakes::FakeClock clock_{testing::noon};
  testing::ProbeScript script_;
  FakeProbe probe_{clock_, script_};
  RecordingSink sink_;
  Watchdog watchdog_{probe_, clock_, sink_};
};

TEST_F(WatchdogTest, IdleWithoutEvents) {
  EXPECT_EQ(watchdog().poll(), std::nullopt);
  EXPECT_TRUE(script().probed.empty());
}

TEST_F(WatchdogTest, FastCopyRecordsNothing) {
  key(Chord::copy);
  run_for(40ms);
  watchdog().clipboard_updated(now());
  EXPECT_EQ(watchdog().poll(), std::nullopt);
  EXPECT_TRUE(sink().incidents.empty());
}

TEST_F(WatchdogTest, CopyAtThresholdRecordsNothing) {
  key(Chord::copy);
  run_for(250ms);
  watchdog().clipboard_updated(now());
  run();
  EXPECT_TRUE(sink().incidents.empty());
}

TEST_F(WatchdogTest, SlowCopyRecordsIncidentWithContext) {
  script().cpu_readings = {{.idle = 1000, .busy = 1000}, {.idle = 1200, .busy = 1800}};
  script().memory_load = 71;
  script().viewer_window = std::nullopt;
  script().clipboard_formats = {{.id = 13}, {.id = 1}};
  script().hung = {{.pid = 3000, .image = "Hung.exe"}};
  script().running = {"explorer.exe", "AnyDesk.exe"};
  const WallTime pressed = clock().now();
  key(Chord::copy, 16ms);
  run_for(400ms);
  // The copier owns the clipboard after the update.
  script().owner_window = window(0x300, 30, 3100, "Substance.exe");
  watchdog().clipboard_updated(now());
  EXPECT_EQ(watchdog().poll(), std::nullopt);

  ASSERT_EQ(sink().incidents.size(), 1U);
  const Incident& incident = sink().incidents[0];
  EXPECT_EQ(incident.chord, Chord::copy);
  EXPECT_EQ(incident.time, pressed);
  EXPECT_EQ(incident.latency, 400ms);
  EXPECT_FALSE(incident.capped);
  EXPECT_EQ(incident.owner_unanswered, std::nullopt);
  EXPECT_EQ(incident.input_lag, 16ms);
  EXPECT_EQ(incident.foreground, (ProcessRef{.pid = 1000, .image = "Code.exe"}));
  EXPECT_EQ(incident.owner, (ProcessRef{.pid = 3100, .image = "Substance.exe"}));
  EXPECT_EQ(incident.holder, std::nullopt);
  EXPECT_EQ(incident.viewer, std::nullopt);
  const std::vector<ClipboardFormat> formats = {
      {.id = 13, .name = "CF_UNICODETEXT", .synthesized = false},
      {.id = 1, .name = "CF_TEXT", .synthesized = true}};
  EXPECT_EQ(incident.formats, formats);
  EXPECT_EQ(incident.hung, script().hung);
  EXPECT_EQ(incident.suspects, std::vector<std::string>{"AnyDesk.exe"});
  EXPECT_EQ(incident.cpu_percent, 80.0);
  EXPECT_EQ(incident.memory_load_percent, 71);
  // A copy never probes windows with WM_NULL.
  EXPECT_TRUE(script().probed.empty());
}

TEST_F(WatchdogTest, CopyWithoutUpdateIsDroppedAfterTheLimit) {
  const MonoTime start = now();
  key(Chord::copy);
  run();
  EXPECT_TRUE(sink().incidents.empty());
  EXPECT_EQ(now() - start, watch_limit);
  // A late update is not attributed to the dropped copy.
  watchdog().clipboard_updated(now());
  EXPECT_EQ(watchdog().poll(), std::nullopt);
  EXPECT_TRUE(sink().incidents.empty());
}

TEST_F(WatchdogTest, NewCtrlCRestartsTheCopy) {
  key(Chord::copy);
  run_for(300ms);
  key(Chord::copy);
  run_for(100ms);
  watchdog().clipboard_updated(now());
  run();
  EXPECT_TRUE(sink().incidents.empty());
}

TEST_F(WatchdogTest, UpdatedCopyIsFinishedBeforeTheNextCtrlCRestarts) {
  key(Chord::copy);
  clock().advance(300ms);  // the probing thread was busy while all three events came
  watchdog().clipboard_updated(now());
  key(Chord::copy);
  run();
  ASSERT_EQ(sink().incidents.size(), 1U);
  EXPECT_EQ(sink().incidents[0].latency, 300ms);
}

TEST_F(WatchdogTest, UpdateWithoutCtrlCIsIgnored) {
  watchdog().clipboard_updated(now());
  EXPECT_EQ(watchdog().poll(), std::nullopt);
  EXPECT_TRUE(sink().incidents.empty());
}

TEST_F(WatchdogTest, CopyEndedBeforeItsFirstRoundStillNamesTheForeground) {
  key(Chord::copy);
  clock().advance(300ms);  // the probing thread was busy
  watchdog().clipboard_updated(now());
  run();
  ASSERT_EQ(sink().incidents.size(), 1U);
  EXPECT_EQ(sink().incidents[0].latency, 300ms);
  EXPECT_EQ(sink().incidents[0].foreground, (ProcessRef{.pid = 1000, .image = "Code.exe"}));
  EXPECT_EQ(sink().incidents[0].cpu_percent, std::nullopt);
}

TEST_F(WatchdogTest, ResponsivePasteNeedsOneProbe) {
  key(Chord::paste);
  EXPECT_EQ(run(), 1);
  EXPECT_EQ(script().probed, std::vector<std::uint64_t>{editor});
  EXPECT_TRUE(sink().incidents.empty());
}

TEST_F(WatchdogTest, PasteStallIsTimeToFirstAnswer) {
  script().answers_from[editor] = now() + 800ms;
  key(Chord::paste);
  run();
  ASSERT_EQ(sink().incidents.size(), 1U);
  const Incident& incident = sink().incidents[0];
  EXPECT_EQ(incident.chord, Chord::paste);
  EXPECT_EQ(incident.latency, 800ms);
  EXPECT_FALSE(incident.capped);
  // Owner and foreground are one thread: the owner is named, not probed separately.
  EXPECT_EQ(incident.owner_unanswered, std::nullopt);
  EXPECT_EQ(incident.owner, (ProcessRef{.pid = 1000, .image = "Code.exe"}));
  // 8 probes timed out every 100 ms, the 9th answered.
  EXPECT_EQ(script().probed.size(), 9U);
}

TEST_F(WatchdogTest, PasteThatNeverAnswersStopsAtTheLimit) {
  script().answers_from[editor] = now() + 1h;
  const MonoTime start = now();
  key(Chord::paste);
  run();
  ASSERT_EQ(sink().incidents.size(), 1U);
  EXPECT_EQ(sink().incidents[0].latency, watch_limit);
  EXPECT_TRUE(sink().incidents[0].capped);
  EXPECT_EQ(now() - start, watch_limit);
  EXPECT_EQ(script().probed.size(), 50U);
}

TEST_F(WatchdogTest, BusyOwnerWhilePasterAnswersIsAnIncident) {
  owner_in_other_process(700ms);
  key(Chord::paste);
  run();
  ASSERT_EQ(sink().incidents.size(), 1U);
  const Incident& incident = sink().incidents[0];
  EXPECT_EQ(incident.latency, 0ms);
  EXPECT_EQ(incident.owner_unanswered, 700ms);
  EXPECT_EQ(incident.owner, (ProcessRef{.pid = 2000, .image = "Blender.exe"}));
  EXPECT_EQ(classify(incident).mechanism, Mechanism::delayed_rendering);
}

TEST_F(WatchdogTest, ResponsiveOwnerInOtherThreadIsProbedOnce) {
  owner_in_other_process(0ms);
  key(Chord::paste);
  run();
  EXPECT_EQ(script().probed, (std::vector<std::uint64_t>{editor, source}));
  EXPECT_TRUE(sink().incidents.empty());
}

TEST_F(WatchdogTest, OwnerThatNeverAnswersIsRecordedAtTheLimit) {
  owner_in_other_process(1h);
  key(Chord::paste);
  run();
  ASSERT_EQ(sink().incidents.size(), 1U);
  EXPECT_EQ(sink().incidents[0].owner_unanswered, watch_limit);
  EXPECT_FALSE(sink().incidents[0].capped);
}

TEST_F(WatchdogTest, BothHungAreProbedInTurn) {
  script().answers_from[editor] = now() + 1h;
  owner_in_other_process(1h);
  key(Chord::paste);
  run();
  ASSERT_EQ(sink().incidents.size(), 1U);
  EXPECT_TRUE(sink().incidents[0].capped);
  EXPECT_EQ(sink().incidents[0].owner_unanswered, watch_limit);
  // Each round takes 200 ms: 25 rounds of two probes.
  EXPECT_EQ(script().probed.size(), 50U);
}

TEST_F(WatchdogTest, PasteWithoutForegroundWindowOnlyWatchesTheOwner) {
  script().foreground_window = std::nullopt;
  owner_in_other_process(400ms);
  key(Chord::paste);
  run();
  ASSERT_EQ(sink().incidents.size(), 1U);
  EXPECT_EQ(sink().incidents[0].latency, 0ms);
  EXPECT_EQ(sink().incidents[0].foreground, std::nullopt);
  EXPECT_EQ(sink().incidents[0].owner_unanswered, 400ms);
}

TEST_F(WatchdogTest, InputLagAloneMakesAnIncident) {
  key(Chord::paste, 600ms);
  run();
  ASSERT_EQ(sink().incidents.size(), 1U);
  EXPECT_EQ(sink().incidents[0].input_lag, 600ms);
  EXPECT_EQ(classify(sink().incidents[0]).mechanism, Mechanism::slow_keyboard_hook);
}

TEST_F(WatchdogTest, CtrlVDuringAPasteWatchIsIgnored) {
  script().answers_from[editor] = now() + 600ms;
  const MonoTime start = now();
  key(Chord::paste);
  run_for(300ms);
  key(Chord::paste);
  run();
  ASSERT_EQ(sink().incidents.size(), 1U);
  EXPECT_EQ(sink().incidents[0].latency, 600ms);
  EXPECT_EQ(now() - start, 600ms);
}

TEST_F(WatchdogTest, ForeignHolderIsRecordedAndTheForegroundsOwnOpenIsNot) {
  script().open_window = window(editor, 10, 1000, "Code.exe");
  key(Chord::copy);
  run_for(100ms);
  script().open_window = window(0x400, 40, 4000, "AnyDesk.exe");
  run_for(100ms);
  script().open_window = window(0x500, 50, 5000, "Other.exe");
  run_for(200ms);
  watchdog().clipboard_updated(now());
  run();
  ASSERT_EQ(sink().incidents.size(), 1U);
  // The first foreign holder seen is kept.
  EXPECT_EQ(sink().incidents[0].holder, (ProcessRef{.pid = 4000, .image = "AnyDesk.exe"}));
  EXPECT_EQ(classify(sink().incidents[0]).mechanism, Mechanism::clipboard_held_open);
}

TEST_F(WatchdogTest, OwnOpenDuringCopyIsNoHolder) {
  script().open_window = window(editor, 10, 1000, "Code.exe");
  key(Chord::copy);
  run_for(300ms);
  watchdog().clipboard_updated(now());
  run();
  ASSERT_EQ(sink().incidents.size(), 1U);
  EXPECT_EQ(sink().incidents[0].holder, std::nullopt);
}

TEST_F(WatchdogTest, CopyAndPasteAreWatchedTogether) {
  script().answers_from[editor] = now() + 500ms;
  key(Chord::paste);
  run_for(100ms);
  const MonoTime copied = now();
  key(Chord::copy);
  run_for(300ms);
  watchdog().clipboard_updated(now());
  const MonoTime updated = now();
  run();
  ASSERT_EQ(sink().incidents.size(), 2U);
  for (const Incident& incident : sink().incidents) {
    if (incident.chord == Chord::copy) {
      EXPECT_EQ(incident.latency, updated - copied);
    } else {
      EXPECT_EQ(incident.latency, 500ms);
    }
  }
  EXPECT_NE(sink().incidents[0].chord, sink().incidents[1].chord);
}

TEST_F(WatchdogTest, ProbeRoundsComeEveryPeriod) {
  key(Chord::copy);
  EXPECT_EQ(watchdog().poll(), now() + probe_period);
  clock().advance(30ms);
  // Not due yet: the poll does no round and keeps the schedule.
  EXPECT_EQ(watchdog().poll(), now() + 70ms);
}

TEST_F(WatchdogTest, EventsFromAnotherThreadAreQueued) {
  script().answers_from[editor] = now() + 300ms;
  std::thread input([&] { key(Chord::paste); });
  input.join();
  run();
  ASSERT_EQ(sink().incidents.size(), 1U);
  EXPECT_EQ(sink().incidents[0].latency, 300ms);
}

}  // namespace
}  // namespace myclean::input
