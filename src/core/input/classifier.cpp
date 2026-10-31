#include "core/input/classifier.hpp"

#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/input/incident.hpp"
#include "core/model/enum_names.hpp"

namespace myclean::input {

namespace {

constexpr std::array<std::string_view, 6> mechanism_names = {
    "delayed_rendering",  "clipboard_held_open", "viewer_chain",
    "slow_keyboard_hook", "heavy_payload",       "unexplained"};

std::string name(const ProcessRef& process) {
  if (process.image.empty()) {
    return std::format("pid {}", process.pid);
  }
  return std::format("{} (pid {})", process.image, process.pid);
}

std::string_view key_name(Chord chord) {
  return chord == Chord::copy ? "Ctrl+C" : "Ctrl+V";
}

std::string join(const std::vector<std::string>& items) {
  std::string text;
  for (const std::string& item : items) {
    text += text.empty() ? item : ", " + item;
  }
  return text;
}

}  // namespace

std::string_view to_string(Mechanism mechanism) {
  return detail::enum_name(mechanism_names, mechanism);
}

std::optional<Mechanism> parse_mechanism(std::string_view text) {
  return detail::enum_from_name<Mechanism>(mechanism_names, text);
}

Classification classify(const Incident& incident) {
  if (incident.chord == Chord::paste && incident.owner && incident.owner_unanswered &&
      *incident.owner_unanswered >= probe_timeout) {
    return {.mechanism = Mechanism::delayed_rendering,
            .process = incident.owner,
            .evidence = {std::format("clipboard owner {} did not answer WM_NULL for {} ms after {}",
                                     name(*incident.owner), incident.owner_unanswered->count(),
                                     key_name(incident.chord))}};
  }
  if (incident.holder) {
    return {.mechanism = Mechanism::clipboard_held_open,
            .process = incident.holder,
            .evidence = {std::format("{} held the clipboard open during the {} ms stall",
                                     name(*incident.holder), incident.latency.count())}};
  }
  if (incident.chord == Chord::copy && incident.viewer) {
    return {.mechanism = Mechanism::viewer_chain,
            .process = incident.viewer,
            .evidence = {
                std::format("{} heads the legacy clipboard viewer chain", name(*incident.viewer))}};
  }
  if (incident.input_lag > incident_threshold) {
    Classification result{
        .mechanism = Mechanism::slow_keyboard_hook,
        .evidence = {std::format("{} reached the watchdog {} ms after its input time",
                                 key_name(incident.chord), incident.input_lag.count())}};
    if (!incident.suspects.empty()) {
      result.evidence.push_back("suspects running: " + join(incident.suspects));
    }
    return result;
  }
  if (incident.chord == Chord::copy) {
    std::vector<std::string> heavy;
    for (const ClipboardFormat& format : incident.formats) {
      if (!format.synthesized && is_heavy(format)) {
        heavy.push_back(format.name.empty() ? std::to_string(format.id) : format.name);
      }
    }
    if (!heavy.empty()) {
      return {.mechanism = Mechanism::heavy_payload,
              .process = incident.owner ? incident.owner : incident.foreground,
              .evidence = {"copied formats include " + join(heavy)}};
    }
  }
  return {.mechanism = Mechanism::unexplained, .evidence = {"no fingerprint matched"}};
}

}  // namespace myclean::input
