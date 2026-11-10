#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/startup/impact.hpp"

// Windows' own boot measurements: the events of Microsoft-Windows-Diagnostics-Performance/
// Operational, which the Diagnostic Policy Service writes after each boot. Only SYSTEM,
// elevated administrators and service logons can read that log (spike X6), so the service reads
// it (IDiagnosticsLog, src/platform/diagnostics) and everything else asks the service. Event 101
// ("an application slowed the boot") is the impact shown for a startup item before MyClean has
// measured a logon with it (startup spec, "Fallback before history exists").
namespace myclean::startup {

// How far back events 101 count for the impact of a startup item.
inline constexpr std::chrono::days delay_span{30};

// One event as EvtRender renders it (EvtRenderEventXml), reduced to what MyClean reads.
struct DiagnosticsEvent {
  std::uint32_t id = 0;             // System/EventID
  std::optional<WallTime> created;  // System/TimeCreated/@SystemTime
  // EventData/Data as (@Name, text), in document order; both with XML character references
  // decoded.
  std::vector<std::pair<std::string, std::string>> data;

  // The text of the first Data named `name`; nullopt when there is none.
  [[nodiscard]] std::optional<std::string_view> value(std::string_view name) const;
};

// Parses one rendered event. Fails with invalid_data when it has no numeric EventID.
[[nodiscard]] Result<DiagnosticsEvent> parse_event(std::string_view xml);

// An xs:dateTime in UTC as the event log renders it: "2026-11-09T08:00:05.1234567Z" (0 to 9
// fraction digits, a trailing Z). nullopt for any other text.
[[nodiscard]] std::optional<WallTime> parse_utc(std::string_view text);

// The BootDelay of an event 101: its Name (or the file name of its Path when Name is empty),
// lower-case, and its DegradationTime, the delay Windows attributed to the program. nullopt for
// another event or one without a name or a degradation time.
[[nodiscard]] std::optional<BootDelay> boot_delay(const DiagnosticsEvent& event);

// The delays of the events 101 among `events` (rendered XML) created at or after `since`;
// events that do not parse are left out.
[[nodiscard]] std::vector<BootDelay> boot_delays(std::span<const std::string> events,
                                                 WallTime since);

// The Diagnostics-Performance/Operational log. Implemented by src/platform/diagnostics; tests
// use fakes.
class IDiagnosticsLog {
 public:
  virtual ~IDiagnosticsLog() = default;
  IDiagnosticsLog(const IDiagnosticsLog&) = delete;
  IDiagnosticsLog& operator=(const IDiagnosticsLog&) = delete;
  IDiagnosticsLog(IDiagnosticsLog&&) = delete;
  IDiagnosticsLog& operator=(IDiagnosticsLog&&) = delete;

  // Events 100 and 101, newest first, each rendered as XML (UTF-8). Fails with access_denied
  // when this process may not read the log (any user but SYSTEM and elevated administrators).
  [[nodiscard]] virtual Result<std::vector<std::string>> events() const = 0;

 protected:
  IDiagnosticsLog() = default;
};

}  // namespace myclean::startup
