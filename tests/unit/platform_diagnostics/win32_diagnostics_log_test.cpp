// Win32DiagnosticsLog against this machine: the log is only read. Run elevated (or as SYSTEM),
// every event it returns parses as an event 100 or 101; run as a plain user, the channel's DACL
// refuses the query and the reader reports access_denied, not an empty log.

#include "platform/diagnostics/win32_diagnostics_log.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/startup/diagnostics.hpp"

namespace myclean::platform {
namespace {

TEST(Win32DiagnosticsLogTest, ReadsEvents100And101OrReportsThatItMayNot) {
  const Win32DiagnosticsLog log;
  const Result<std::vector<std::string>> events = log.events();
  if (!events) {
    EXPECT_EQ(events.error().code, Errc::access_denied) << events.error().message;
    return;
  }
  EXPECT_LE(events->size(), Win32DiagnosticsLog::max_events);
  for (const std::string& xml : *events) {
    const Result<startup::DiagnosticsEvent> event = startup::parse_event(xml);
    ASSERT_TRUE(event.has_value()) << xml;
    EXPECT_TRUE(event->id == 100 || event->id == 101) << xml;
    EXPECT_TRUE(event->created.has_value()) << xml;
  }
}

}  // namespace
}  // namespace myclean::platform
