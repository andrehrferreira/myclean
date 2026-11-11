#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/units.hpp"
#include "core/security/persistence.hpp"
#include "core/trust/trust.hpp"

// The intruder scan's judgment of one persistence point (security spec, "Verdicts Only From
// Defender Or Opt-In VirusTotal"): a verdict only from Defender or VirusTotal, otherwise
// "suspicious because <reasons>" with the concrete reasons, never "dangerous".
namespace myclean::security {

// Why a point is suspicious.
enum class Suspicion {
  unsigned_image,         // "unsigned"
  invalid_signature,      // "invalid-signature"
  unlisted_signer,        // "signer-not-listed"
  user_writable_path,     // "user-writable-path"
  not_in_knowledge_base,  // "not-in-knowledge-base"
  target_missing,         // "target-missing": the file it runs does not exist
  winlogon_changed,       // "non-default-winlogon"
  appinit_dll,            // "appinit-dll"
  ifeo_debugger,          // "ifeo-debugger"
  wmi_consumer,           // "wmi-consumer"
};

[[nodiscard]] std::string_view to_string(Suspicion suspicion);
// The phrase of "suspicious because <phrase>, <phrase>": "unsigned", "runs from a user-writable
// path", "not in the knowledge base", ...
[[nodiscard]] std::string_view phrase(Suspicion suspicion);

enum class Status {
  standard,         // "default": Windows' own value
  ok,               // "ok": nothing unusual
  suspicious,       // "suspicious": see the reasons
  known_dangerous,  // "known_dangerous": Defender or VirusTotal says so
};

[[nodiscard]] std::string_view to_string(Status status);

struct Verdict {
  std::string source;                    // defender_source or virustotal_source
  std::string threat;                    // Defender's threat name; empty for VirusTotal
  std::optional<std::uint32_t> flagged;  // VirusTotal: engines that flag it
  std::optional<std::uint32_t> engines;  // VirusTotal: engines that answered
  std::optional<WallTime> time;
};

struct Finding {
  Point point;
  std::optional<std::filesystem::path> image;  // the target resolved, when the file exists
  std::optional<trust::TrustSignal> trust;     // nullopt: no image, or it could not be read
  std::optional<Verdict> verdict;
  std::vector<Suspicion> reasons;
  Status status = Status::ok;
};

// The finding of `point`: standard points are `standard` with no reasons. Otherwise the reasons
// are the point's concern, target_missing when it names a file that `image` did not resolve,
// and the trust signal's reasons (its signature reason, the user-writable path, then
// not_in_knowledge_base). A verdict makes it known_dangerous whatever the reasons; else any
// reason makes it suspicious.
[[nodiscard]] Finding assess(Point point, std::optional<std::filesystem::path> image,
                             std::optional<trust::TrustSignal> trust,
                             std::optional<Verdict> verdict);

// One line for the owner: "default Windows value", "nothing unusual", "suspicious because
// unsigned, runs from a user-writable path, not in the knowledge base", "known dangerous:
// Microsoft Defender detected Trojan:Win32/X" or "known dangerous: VirusTotal, flagged by 5 of
// 70 engines".
[[nodiscard]] std::string summary(const Finding& finding);

}  // namespace myclean::security
