#include "core/security/verdicts.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/security/persistence.hpp"
#include "core/security/scan.hpp"
#include "core/trust/trust.hpp"

namespace myclean::security {
namespace {

using namespace std::chrono_literals;

const WallTime noon = std::chrono::sys_days{std::chrono::November / 10 / 2026} + 12h;

TEST(HashReport, CountsTheEnginesThatAnswered) {
  const Result<HashReport> report = parse_hash_report(R"({"data": {"attributes": {
      "last_analysis_stats": {"malicious": 5, "suspicious": 1, "undetected": 60, "harmless": 4,
                              "timeout": 2, "type-unsupported": 3, "failure": 1}}}})");
  ASSERT_TRUE(report) << report.error().message;
  EXPECT_EQ(report->malicious, 5U);
  EXPECT_EQ(report->engines, 70U);
  EXPECT_FALSE(parse_hash_report("not json"));
  constexpr std::string_view not_found = R"({"error": {"code": "NotFoundError"}})";
  EXPECT_FALSE(parse_hash_report(not_found));
}

TEST(FileScan, ReadsMpCmdRunsExitCodeAndThreat) {
  const Result<FileScan> clean = parse_file_scan(0, "Scan starting...\nScan finished.\n");
  ASSERT_TRUE(clean);
  EXPECT_FALSE(clean->detected);
  const Result<FileScan> found = parse_file_scan(
      2,
      "Scan starting...\r\nScan finished.\r\nScanning C:\\x\\eicar.com found 1 threats.\r\n\r\n"
      "<===========================LIST OF DETECTED THREATS==========================>\r\n"
      "----------------------------- Threat information ------------------------------\r\n"
      "Threat                  : Virus:DOS/EICAR_Test_File\r\n"
      "Resources               : 1 total\r\n    file                : C:\\x\\eicar.com\r\n");
  ASSERT_TRUE(found);
  EXPECT_TRUE(found->detected);
  EXPECT_EQ(found->threat, "Virus:DOS/EICAR_Test_File");
  EXPECT_FALSE(parse_file_scan(5, "CmdTool: Failed with hr = 0x80508023"));
}

TEST(CimDatetime, ParsesWithItsOffset) {
  EXPECT_EQ(parse_cim_datetime("20261110120000.000000+000"), noon);
  EXPECT_EQ(parse_cim_datetime("20261110093000.500000-150"), noon + 500ms);
  EXPECT_FALSE(parse_cim_datetime("2026-11-10"));
  EXPECT_FALSE(parse_cim_datetime("20261340120000.000000+000"));
}

TEST(DetectionOf, MatchesFileResourcesIgnoringCase) {
  const std::vector<Detection> detections = {
      {.threat = "Old", .resources = {R"(file:_C:\Users\a\x.exe)"}, .time = noon - 48h},
      {.threat = "New",
       .resources = {R"(regkey:_HKCU\Run\\x)", R"(file:_c:\users\a\X.EXE)"},
       .time = noon},
      {.threat = "Zip", .resources = {R"(containerfile:_C:\d\a.zip->(Zip))"}, .time = noon}};
  const Detection* found = detection_of(detections, R"(C:\Users\a\x.exe)");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->threat, "New");
  ASSERT_NE(detection_of(detections, R"(C:\d\a.zip)"), nullptr);
  EXPECT_EQ(detection_of(detections, R"(C:\Users\a\y.exe)"), nullptr);
}

TEST(Hex, IsLowerCase) {
  const std::array<std::uint8_t, 3> digest = {0x00, 0xAB, 0x7F};
  EXPECT_EQ(hex(digest), "00ab7f");
}

TEST(Settings, OffByDefault) {
  const Result<Settings> settings = parse_settings("");
  ASSERT_TRUE(settings);
  EXPECT_FALSE(settings->reverse_dns);
  EXPECT_FALSE(settings->virustotal);
  EXPECT_TRUE(settings->virustotal_note.empty());
}

// Spec: "No API key keeps VirusTotal off".
TEST(Settings, VirusTotalWithoutAKeyStaysOff) {
  const Result<Settings> settings = parse_settings("[virustotal]\nenabled = true\n");
  ASSERT_TRUE(settings);
  EXPECT_FALSE(settings->virustotal);
  EXPECT_NE(settings->virustotal_note.find("API key"), std::string::npos);
  const Result<Settings> bad = parse_settings("[virustotal]\nenabled = true\napi_key = \"a b\"\n");
  ASSERT_TRUE(bad);
  EXPECT_FALSE(bad->virustotal);
}

TEST(Settings, EnabledWithAKey) {
  const Result<Settings> settings = parse_settings(
      "[network]\nreverse_dns = true\n[virustotal]\nenabled = true\napi_key = \"abc123\"\n");
  ASSERT_TRUE(settings);
  EXPECT_TRUE(settings->reverse_dns);
  EXPECT_TRUE(settings->virustotal);
  EXPECT_EQ(settings->api_key, "abc123");
  EXPECT_FALSE(parse_settings("[network]\nreverse_dns = \"yes\"\n"));
  EXPECT_FALSE(parse_settings("[network"));
}

Point run_point(const std::string& target) {
  return Point{.source = PointSource::run,
               .location = R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Run)",
               .name = "Updater",
               .command = target,
               .target = target,
               .enabled = true};
}

trust::TrustSignal unsigned_in_temp(const std::filesystem::path& image) {
  return trust::assess(image, trust::Signature{}, trust::PathClass::user_writable, {});
}

// Spec: "Unsigned binary without verdict is only suspicious".
TEST(Assess, UnsignedBinaryInTempIsOnlySuspicious) {
  const std::filesystem::path image = R"(C:\Users\a\AppData\Local\Temp\upd.exe)";
  const Finding finding =
      assess(run_point(image.string()), image, unsigned_in_temp(image), std::nullopt);
  EXPECT_EQ(finding.status, Status::suspicious);
  EXPECT_EQ(summary(finding),
            "suspicious because unsigned, runs from a user-writable path, not in the knowledge "
            "base");
  EXPECT_EQ(summary(finding).find("dangerous"), std::string::npos);
}

// Spec: "Defender detection produces a verdict".
TEST(Assess, DefenderDetectionIsKnownDangerous) {
  const std::filesystem::path image = R"(C:\Users\a\AppData\Local\Temp\upd.exe)";
  const Finding finding = assess(
      run_point(image.string()), image, unsigned_in_temp(image),
      Verdict{.source = std::string(defender_source), .threat = "Trojan:Win32/Test", .time = noon});
  EXPECT_EQ(finding.status, Status::known_dangerous);
  EXPECT_EQ(summary(finding), "known dangerous: Microsoft Defender detected Trojan:Win32/Test");
}

TEST(Assess, VirusTotalVerdictSaysHowManyEngines) {
  const Finding finding =
      assess(run_point(R"(C:\x.exe)"), std::filesystem::path(R"(C:\x.exe)"), std::nullopt,
             Verdict{.source = std::string(virustotal_source), .flagged = 5, .engines = 70});
  EXPECT_EQ(summary(finding), "known dangerous: VirusTotal, flagged by 5 of 70 engines");
}

TEST(Assess, StandardAndMissingTargets) {
  Point standard = run_point("explorer.exe");
  standard.standard = true;
  EXPECT_EQ(assess(standard, std::nullopt, std::nullopt, std::nullopt).status, Status::standard);

  const Finding missing =
      assess(run_point(R"(C:\gone\x.exe)"), std::nullopt, std::nullopt, std::nullopt);
  EXPECT_EQ(missing.status, Status::suspicious);
  EXPECT_EQ(summary(missing), "suspicious because the file it runs does not exist");

  Point debugger = run_point(R"(C:\Tools\np.exe)");
  debugger.concern = Concern::ifeo_debugger;
  const trust::TrustSignal known =
      trust::assess(R"(C:\Tools\np.exe)",
                    trust::Signature{.status = trust::SignatureStatus::valid, .signer = "X"},
                    trust::PathClass::protected_location,
                    std::vector<rules::KbEntry>{{.id = "np", .image = "np.exe", .signer = "X"}});
  const Finding ifeo =
      assess(debugger, std::filesystem::path(R"(C:\Tools\np.exe)"), known, std::nullopt);
  ASSERT_EQ(ifeo.reasons.size(), 1U);
  EXPECT_EQ(ifeo.reasons[0], Suspicion::ifeo_debugger);

  const Finding fine = assess(run_point(R"(C:\Tools\np.exe)"),
                              std::filesystem::path(R"(C:\Tools\np.exe)"), known, std::nullopt);
  EXPECT_EQ(fine.status, Status::ok);
  EXPECT_EQ(summary(fine), "nothing unusual");
}

}  // namespace
}  // namespace myclean::security
