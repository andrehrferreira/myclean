#include "core/input/classifier.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/input/incident.hpp"

namespace myclean::input {
namespace {

using namespace std::chrono_literals;

const ProcessRef paster{.pid = 1000, .image = "Code.exe"};
const ProcessRef copier{.pid = 2000, .image = "Blender.exe"};
const ProcessRef remote{.pid = 4000, .image = "AnyDesk.exe"};
const ProcessRef viewer{.pid = 5000, .image = "OldViewer.exe"};

Incident paste() {
  return {.chord = Chord::paste, .latency = 600ms, .foreground = paster, .owner = copier};
}

Incident copy() {
  return {.chord = Chord::copy, .latency = 600ms, .foreground = copier, .owner = copier};
}

TEST(Mechanism, TextFormsRoundTrip) {
  for (const Mechanism mechanism :
       {Mechanism::delayed_rendering, Mechanism::clipboard_held_open, Mechanism::viewer_chain,
        Mechanism::slow_keyboard_hook, Mechanism::heavy_payload, Mechanism::unexplained}) {
    EXPECT_EQ(parse_mechanism(to_string(mechanism)), mechanism);
  }
  EXPECT_EQ(to_string(Mechanism::clipboard_held_open), "clipboard_held_open");
  EXPECT_EQ(parse_mechanism("hook"), std::nullopt);
}

TEST(Classify, OwnerNotAnsweringOnPasteIsDelayedRendering) {
  Incident incident = paste();
  incident.owner_unanswered = 700ms;
  const Classification result = classify(incident);
  EXPECT_EQ(result.mechanism, Mechanism::delayed_rendering);
  EXPECT_EQ(result.process, copier);
  EXPECT_EQ(result.evidence, std::vector<std::string>{
                                 "clipboard owner Blender.exe (pid 2000) did not answer WM_NULL "
                                 "for 700 ms after Ctrl+V"});
}

TEST(Classify, OwnerThatAnsweredTheFirstProbeIsNotDelayedRendering) {
  Incident incident = paste();
  incident.owner_unanswered = 99ms;
  EXPECT_EQ(classify(incident).mechanism, Mechanism::unexplained);
  incident.owner_unanswered = probe_timeout;
  EXPECT_EQ(classify(incident).mechanism, Mechanism::delayed_rendering);
}

TEST(Classify, ForeignHolderIsClipboardHeldOpen) {
  for (Incident incident : {paste(), copy()}) {
    incident.holder = remote;
    const Classification result = classify(incident);
    EXPECT_EQ(result.mechanism, Mechanism::clipboard_held_open);
    EXPECT_EQ(result.process, remote);
    EXPECT_EQ(result.evidence,
              std::vector<std::string>{"AnyDesk.exe (pid 4000) held the clipboard open during the "
                                       "600 ms stall"});
  }
}

TEST(Classify, ViewerChainCountsOnCopyOnly) {
  Incident incident = copy();
  incident.viewer = viewer;
  const Classification result = classify(incident);
  EXPECT_EQ(result.mechanism, Mechanism::viewer_chain);
  EXPECT_EQ(result.process, viewer);
  EXPECT_EQ(result.evidence,
            std::vector<std::string>{"OldViewer.exe (pid 5000) heads the legacy clipboard viewer "
                                     "chain"});

  Incident on_paste = paste();
  on_paste.viewer = viewer;
  EXPECT_EQ(classify(on_paste).mechanism, Mechanism::unexplained);
}

TEST(Classify, InputLagIsSlowKeyboardHookWithSuspectsAsEvidence) {
  Incident incident = copy();
  incident.input_lag = 600ms;
  incident.suspects = {"PowerToys.KeyboardManagerEngine.exe", "Discord.exe"};
  const Classification result = classify(incident);
  EXPECT_EQ(result.mechanism, Mechanism::slow_keyboard_hook);
  EXPECT_EQ(result.process, std::nullopt);
  const std::vector<std::string> evidence = {
      "Ctrl+C reached the watchdog 600 ms after its input time",
      "suspects running: PowerToys.KeyboardManagerEngine.exe, Discord.exe"};
  EXPECT_EQ(result.evidence, evidence);

  incident.input_lag = incident_threshold;
  EXPECT_NE(classify(incident).mechanism, Mechanism::slow_keyboard_hook);
}

TEST(Classify, HeavyFormatOnCopyIsHeavyPayload) {
  Incident incident = copy();
  incident.formats = describe_formats({{.id = 0xC100, .name = "PNG"}, {.id = 8}, {.id = 17}});
  const Classification result = classify(incident);
  EXPECT_EQ(result.mechanism, Mechanism::heavy_payload);
  EXPECT_EQ(result.process, copier);
  // CF_DIBV5 is synthesized from CF_DIB and not listed twice.
  EXPECT_EQ(result.evidence, std::vector<std::string>{"copied formats include PNG, CF_DIB"});
}

TEST(Classify, HeavyPayloadNamesTheForegroundWithoutOwner) {
  Incident incident = copy();
  incident.owner = std::nullopt;
  incident.formats = {{.id = 8, .name = "CF_DIB"}};
  EXPECT_EQ(classify(incident).process, copier);
}

TEST(Classify, SynthesizedHeavyFormatAloneIsNotHeavyPayload) {
  Incident incident = copy();
  incident.formats = describe_formats({{.id = 13}, {.id = 1}});
  EXPECT_EQ(classify(incident).mechanism, Mechanism::unexplained);
  incident.formats = {{.id = 17, .name = "CF_DIBV5", .synthesized = true}};
  EXPECT_EQ(classify(incident).mechanism, Mechanism::unexplained);
}

TEST(Classify, HeavyFormatOnPasteIsNotHeavyPayload) {
  Incident incident = paste();
  incident.formats = {{.id = 0xC100, .name = "PNG"}};
  EXPECT_EQ(classify(incident).mechanism, Mechanism::unexplained);
}

TEST(Classify, NothingMatchedIsUnexplainedWithoutProcess) {
  Incident incident = paste();
  incident.hung = {paster};  // IsHungAppWindow is evidence only, never a fingerprint
  incident.suspects = {"AnyDesk.exe"};
  const Classification result = classify(incident);
  EXPECT_EQ(result.mechanism, Mechanism::unexplained);
  EXPECT_EQ(result.process, std::nullopt);
  EXPECT_EQ(result.evidence, std::vector<std::string>{"no fingerprint matched"});
}

TEST(Classify, FingerprintsAreCheckedInOrder) {
  Incident incident = paste();
  incident.owner_unanswered = 700ms;
  incident.holder = remote;
  incident.input_lag = 600ms;
  EXPECT_EQ(classify(incident).mechanism, Mechanism::delayed_rendering);
  incident.owner_unanswered = std::nullopt;
  EXPECT_EQ(classify(incident).mechanism, Mechanism::clipboard_held_open);

  Incident copied = copy();
  copied.viewer = viewer;
  copied.input_lag = 600ms;
  copied.formats = {{.id = 0xC100, .name = "PNG"}};
  EXPECT_EQ(classify(copied).mechanism, Mechanism::viewer_chain);
  copied.viewer = std::nullopt;
  EXPECT_EQ(classify(copied).mechanism, Mechanism::slow_keyboard_hook);
  copied.input_lag = 0ms;
  EXPECT_EQ(classify(copied).mechanism, Mechanism::heavy_payload);
}

TEST(Classify, UnnamedProcessIsShownByPid) {
  Incident incident = paste();
  incident.holder = ProcessRef{.pid = 77};
  EXPECT_EQ(classify(incident).evidence,
            std::vector<std::string>{"pid 77 held the clipboard open during the 600 ms stall"});
}

}  // namespace
}  // namespace myclean::input
