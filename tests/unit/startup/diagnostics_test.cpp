#include "core/startup/diagnostics.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/startup/impact.hpp"

namespace myclean::startup {
namespace {

using namespace std::chrono_literals;

// An event 101 as EvtRender (EvtRenderEventXml) renders it: single-quoted attributes, the
// System block, then EventData with the fields of the provider's template (version 1, read
// from this machine's Microsoft-Windows-Diagnostics-Performance manifest).
const std::string delay_event =
    R"(<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='Microsoft-Windows-Diagnostics-Performance' Guid='{cfc18ec0-96b1-4eba-961b-622caee05b0a}'/><EventID>101</EventID><Version>1</Version><Level>3</Level><Task>4002</Task><Opcode>33</Opcode><Keywords>0x8000000000010000</Keywords><TimeCreated SystemTime='2026-11-09T08:04:12.6431873Z'/><EventRecordID>4211</EventRecordID><Correlation ActivityID='{a1b2c3d4-0000-0000-0000-000000000001}'/><Execution ProcessID='3120' ThreadID='4388'/><Channel>Microsoft-Windows-Diagnostics-Performance/Operational</Channel><Computer>PC</Computer><Security UserID='S-1-5-19'/></System><EventData><Data Name='StartTime'>2026-11-09T08:00:31.512000000Z</Data><Data Name='NameLength'>12</Data><Data Name='Name'>OneDrive.exe</Data><Data Name='FriendlyNameLength'>17</Data><Data Name='FriendlyName'>Microsoft OneDrive</Data><Data Name='VersionLength'>14</Data><Data Name='Version'>25.199.1012.0002</Data><Data Name='TotalTime'>9351</Data><Data Name='DegradationTime'>4351</Data><Data Name='PathLength'>59</Data><Data Name='Path'>C:\Users\owner\AppData\Local\Microsoft\OneDrive\OneDrive.exe</Data><Data Name='ProductNameLength'>18</Data><Data Name='ProductName'>Microsoft OneDrive</Data><Data Name='CompanyNameLength'>21</Data><Data Name='CompanyName'>Microsoft Corporation</Data></EventData></Event>)";

// Event 101 of a program whose Name is empty, with escaped characters in its fields.
const std::string unnamed_delay_event =
    R"(<Event xmlns="http://schemas.microsoft.com/win/2004/08/events/event"><System><EventID Qualifiers="0">101</EventID><TimeCreated SystemTime="2026-11-01T07:00:00Z"/></System><EventData><Data Name="StartTime">2026-11-01T06:59:00.0000000Z</Data><Data Name="Name"/><Data Name="FriendlyName">Tom &amp; Jerry&#x2019;s &lt;Sync&gt;</Data><Data Name="TotalTime">3000</Data><Data Name="DegradationTime">1250</Data><Data Name="Path">C:\Program Files\T&amp;J\TJSync.EXE</Data></EventData></Event>)";

TEST(ParseUtcTest, ReadsTheTimesTheEventLogRenders) {
  const WallTime day = std::chrono::sys_days{std::chrono::November / 9 / 2026};
  EXPECT_EQ(parse_utc("2026-11-09T08:00:31Z"), day + 8h + 31s);
  EXPECT_EQ(parse_utc("2026-11-09T08:00:31.5Z"), day + 8h + 31s + 500ms);
  EXPECT_EQ(parse_utc("2026-11-09T08:00:31.512000000Z"), day + 8h + 31s + 512ms);
  EXPECT_EQ(parse_utc("2026-11-09T08:04:12.6431873Z"),
            day + 8h + 4min + 12s + std::chrono::duration_cast<WallTime::duration>(643187300ns));
  EXPECT_EQ(parse_utc("2026-11-09T08:00:31"), std::nullopt);  // no Z
  EXPECT_EQ(parse_utc("2026-11-09T08:00:31.Z"), std::nullopt);
  EXPECT_EQ(parse_utc("2026-11-09T08:00:31.1234567891Z"), std::nullopt);
  EXPECT_EQ(parse_utc("2026-02-30T08:00:31Z"), std::nullopt);
  EXPECT_EQ(parse_utc("2026-11-09T24:00:00Z"), std::nullopt);
  EXPECT_EQ(parse_utc("2026-11-09 08:00:31Z"), std::nullopt);
  EXPECT_EQ(parse_utc(""), std::nullopt);
}

TEST(ParseEventTest, ReadsTheIdTheTimeAndTheNamedData) {
  const Result<DiagnosticsEvent> event = parse_event(delay_event);
  ASSERT_TRUE(event.has_value());
  EXPECT_EQ(event->id, 101U);
  EXPECT_EQ(event->created, parse_utc("2026-11-09T08:04:12.6431873Z"));
  EXPECT_EQ(event->data.size(), 15U);
  EXPECT_EQ(event->data.front().first, "StartTime");
  EXPECT_EQ(event->value("Name"), "OneDrive.exe");
  EXPECT_EQ(event->value("DegradationTime"), "4351");
  EXPECT_EQ(event->value("Path"),
            R"(C:\Users\owner\AppData\Local\Microsoft\OneDrive\OneDrive.exe)");
  EXPECT_EQ(event->value("Missing"), std::nullopt);

  const Result<DiagnosticsEvent> unnamed = parse_event(unnamed_delay_event);
  ASSERT_TRUE(unnamed.has_value());
  EXPECT_EQ(unnamed->id, 101U);
  EXPECT_EQ(unnamed->value("Name"), "");
  EXPECT_EQ(unnamed->value("FriendlyName"), "Tom & Jerry\xE2\x80\x99s <Sync>");
}

TEST(ParseEventTest, RefusesAnEventWithoutANumericId) {
  EXPECT_FALSE(
      parse_event("<Event><System><EventRecordID>5</EventRecordID></System></Event>").has_value());
  EXPECT_FALSE(parse_event("<Event><System><EventID>abc</EventID></System></Event>").has_value());
  EXPECT_FALSE(parse_event("").has_value());
}

// Startup spec "Fallback before history exists": the delay Windows attributed to each program.
TEST(BootDelayTest, TakesTheProgramAndTheDegradationTimeOfEvent101) {
  const BootDelay delay = boot_delay(parse_event(delay_event).value()).value_or(BootDelay{});
  EXPECT_EQ(delay.image, "onedrive.exe");
  EXPECT_DOUBLE_EQ(delay.delay_seconds, 4.351);

  const BootDelay unnamed =
      boot_delay(parse_event(unnamed_delay_event).value()).value_or(BootDelay{});
  EXPECT_EQ(unnamed.image, "tjsync.exe");  // from Path
  EXPECT_DOUBLE_EQ(unnamed.delay_seconds, 1.25);

  DiagnosticsEvent other = parse_event(delay_event).value();
  other.id = 102;  // a driver, not a program
  EXPECT_EQ(boot_delay(other), std::nullopt);
  DiagnosticsEvent no_delay = parse_event(delay_event).value();
  std::erase_if(no_delay.data, [](const auto& field) { return field.first == "DegradationTime"; });
  EXPECT_EQ(boot_delay(no_delay), std::nullopt);
}

TEST(BootDelayTest, KeepsOnlyRecentDelayEvents) {
  const WallTime since = std::chrono::sys_days{std::chrono::November / 5 / 2026};
  const std::vector<std::string> events = {delay_event, unnamed_delay_event, "<not an event>"};
  const std::vector<BootDelay> delays = boot_delays(events, since);
  ASSERT_EQ(delays.size(), 1U);  // the unnamed one is from November 1st
  EXPECT_EQ(delays[0].image, "onedrive.exe");
  EXPECT_EQ(boot_delays(events, since - std::chrono::days{10}).size(), 2U);
}

}  // namespace
}  // namespace myclean::startup
