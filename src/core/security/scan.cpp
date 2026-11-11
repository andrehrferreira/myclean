#include "core/security/scan.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/enum_names.hpp"
#include "core/security/persistence.hpp"
#include "core/trust/trust.hpp"

namespace myclean::security {
namespace {

constexpr std::array<std::string_view, 10> suspicion_names = {"unsigned",
                                                              "invalid-signature",
                                                              "signer-not-listed",
                                                              "user-writable-path",
                                                              "not-in-knowledge-base",
                                                              "target-missing",
                                                              "non-default-winlogon",
                                                              "appinit-dll",
                                                              "ifeo-debugger",
                                                              "wmi-consumer"};

constexpr std::array<std::string_view, 10> suspicion_phrases = {
    "unsigned",
    "invalid signature",
    "signed by a signer the knowledge base does not list",
    "runs from a user-writable path",
    "not in the knowledge base",
    "the file it runs does not exist",
    "Winlogon value is not the Windows default",
    "AppInit DLL loaded into every program that uses user32.dll",
    "Image File Execution Options debugger starts instead of the program",
    "WMI event subscription runs a command or script"};

constexpr std::array<std::string_view, 4> status_names = {"default", "ok", "suspicious",
                                                          "known_dangerous"};

Suspicion of(Concern concern) {
  switch (concern) {
    case Concern::winlogon_changed:
      return Suspicion::winlogon_changed;
    case Concern::appinit_dll:
      return Suspicion::appinit_dll;
    case Concern::ifeo_debugger:
      return Suspicion::ifeo_debugger;
    case Concern::wmi_consumer:
      return Suspicion::wmi_consumer;
  }
  return Suspicion::wmi_consumer;
}

std::optional<Suspicion> of(trust::Reason reason) {
  switch (reason) {
    case trust::Reason::unsigned_image:
      return Suspicion::unsigned_image;
    case trust::Reason::invalid_signature:
      return Suspicion::invalid_signature;
    case trust::Reason::unlisted_signer:
      return Suspicion::unlisted_signer;
    case trust::Reason::user_writable_path:
      return Suspicion::user_writable_path;
    case trust::Reason::not_in_knowledge_base:
      return std::nullopt;  // added last
  }
  return std::nullopt;
}

}  // namespace

std::string_view to_string(Suspicion suspicion) {
  return detail::enum_name(suspicion_names, suspicion);
}

std::string_view phrase(Suspicion suspicion) {
  return detail::enum_name(suspicion_phrases, suspicion);
}

std::string_view to_string(Status status) {
  return detail::enum_name(status_names, status);
}

Finding assess(Point point, std::optional<std::filesystem::path> image,
               std::optional<trust::TrustSignal> trust, std::optional<Verdict> verdict) {
  Finding finding{.point = std::move(point),
                  .image = std::move(image),
                  .trust = std::move(trust),
                  .verdict = std::move(verdict)};
  if (finding.point.concern) {
    finding.reasons.push_back(of(*finding.point.concern));
  }
  if (!finding.point.target.empty() && !finding.image) {
    finding.reasons.push_back(Suspicion::target_missing);
  }
  if (finding.trust && finding.trust->unrecognized()) {
    for (const trust::Reason reason : finding.trust->reasons) {
      if (const std::optional<Suspicion> suspicion = of(reason)) {
        finding.reasons.push_back(*suspicion);
      }
    }
    finding.reasons.push_back(Suspicion::not_in_knowledge_base);
  }
  if (finding.verdict) {
    finding.status = Status::known_dangerous;
  } else if (finding.point.standard) {
    finding.status = Status::standard;
    finding.reasons.clear();
  } else {
    finding.status = finding.reasons.empty() ? Status::ok : Status::suspicious;
  }
  return finding;
}

std::string summary(const Finding& finding) {
  switch (finding.status) {
    case Status::standard:
      return "default Windows value";
    case Status::ok:
      return "nothing unusual";
    case Status::suspicious: {
      std::string text = "suspicious because ";
      for (std::size_t index = 0; index < finding.reasons.size(); ++index) {
        text += index == 0 ? "" : ", ";
        text += phrase(finding.reasons[index]);
      }
      return text;
    }
    case Status::known_dangerous: {
      const Verdict& verdict = *finding.verdict;
      if (verdict.flagged && verdict.engines) {
        return "known dangerous: " + verdict.source + ", flagged by " +
               std::to_string(*verdict.flagged) + " of " + std::to_string(*verdict.engines) +
               " engines";
      }
      return "known dangerous: " + verdict.source + " detected " +
             (verdict.threat.empty() ? std::string("a threat") : verdict.threat);
    }
  }
  return {};
}

}  // namespace myclean::security
