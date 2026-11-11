#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

// Verdicts (security spec, "Verdicts Only From Defender Or Opt-In VirusTotal"): an item is
// "known dangerous" only when Microsoft Defender detected it (its detection history, or a file
// scan the owner asked for) or the opt-in VirusTotal hash lookup flags it. Nothing else MyClean
// knows makes a verdict.
namespace myclean::security {

inline constexpr std::string_view defender_source = "Microsoft Defender";
inline constexpr std::string_view virustotal_source = "VirusTotal";

// One entry of Defender's detection history (MSFT_MpThreatDetection, with the ThreatName of
// MSFT_MpThreat).
struct Detection {
  std::string threat;                  // "Trojan:Win32/Wacatac.B!ml"
  std::vector<std::string> resources;  // "file:_C:\Users\x\a.exe", "regkey:_HKLM\...", ...
  std::optional<WallTime> time;        // InitialDetectionTime
};

// The result of an on-demand Defender scan of one file.
struct FileScan {
  bool detected = false;
  std::string threat;  // the first threat MpCmdRun named; empty when none was named
};

// Microsoft Defender. Implemented by src/platform/security; tests use fakes.
class IDefender {
 public:
  virtual ~IDefender() = default;
  IDefender(const IDefender&) = delete;
  IDefender& operator=(const IDefender&) = delete;
  IDefender(IDefender&&) = delete;
  IDefender& operator=(IDefender&&) = delete;

  // The detection history (WMI root\Microsoft\Windows\Defender). Fails when Defender is not
  // the active antivirus or its namespace cannot be read.
  [[nodiscard]] virtual Result<std::vector<Detection>> detections() const = 0;
  // `MpCmdRun.exe -Scan -ScanType 3 -File <file> -DisableRemediation`: reports, and never
  // quarantines or deletes. Blocks until the scan ends; a stop request ends the scan and fails.
  [[nodiscard]] virtual Result<FileScan> scan_file(const std::filesystem::path& file,
                                                   const std::stop_token& stop) = 0;

 protected:
  IDefender() = default;
};

// What VirusTotal knows of one SHA-256.
struct HashReport {
  std::uint32_t malicious = 0;  // engines that flag it as malicious
  std::uint32_t engines = 0;    // engines that answered (malicious, suspicious, undetected,
                                // harmless)
};

// The VirusTotal hash lookup (v3 files/<sha256>). Implemented by src/platform/security; tests use
// fakes. Sends the SHA-256 and the API key, nothing else.
class IHashLookup {
 public:
  virtual ~IHashLookup() = default;
  IHashLookup(const IHashLookup&) = delete;
  IHashLookup& operator=(const IHashLookup&) = delete;
  IHashLookup(IHashLookup&&) = delete;
  IHashLookup& operator=(IHashLookup&&) = delete;

  // nullopt: VirusTotal has no report of the hash.
  [[nodiscard]] virtual Result<std::optional<HashReport>> lookup(std::string_view sha256,
                                                                 std::string_view api_key) = 0;

 protected:
  IHashLookup() = default;
};

// The analysis stats of a VirusTotal v3 file object. Fails with invalid_data when the text is not
// one.
[[nodiscard]] Result<HashReport> parse_hash_report(std::string_view json);

// The outcome of MpCmdRun's file scan from its exit code and output: 0 nothing found, 2 threats
// found (the first "Threat : <name>" line names it); any other code fails with the output.
[[nodiscard]] Result<FileScan> parse_file_scan(int exit_code, std::string_view output);

// A CIM DATETIME ("20261102143005.123456+000", UTC offset in minutes) as WallTime.
[[nodiscard]] std::optional<WallTime> parse_cim_datetime(std::string_view text);

// The newest detection whose file resources name `image` (ignoring ASCII case; "file:_" and
// "containerfile:_" resources, without a "->" archive member); null when none does.
[[nodiscard]] const Detection* detection_of(std::span<const Detection> detections,
                                            const std::filesystem::path& image);

// The lower-case hex text of a SHA-256.
[[nodiscard]] std::string hex(std::span<const std::uint8_t> digest);

// The owner's choices of %ProgramData%\MyClean\security.toml:
//   [network]
//   reverse_dns = false   # resolve the remote addresses of list_network_processes
//   [virustotal]
//   enabled = false       # look up the SHA-256 of suspicious files
//   api_key = ""          # the owner's VirusTotal API key
// Both are off when the file does not exist.
struct Settings {
  bool reverse_dns = false;
  bool virustotal = false;  // enabled and an API key is set
  std::string api_key;
  // Why VirusTotal stays off although enabled; empty otherwise.
  std::string virustotal_note;
};

// The settings of a security.toml text; fails with invalid_data on a TOML error or a value of
// the wrong type. VirusTotal enabled without an API key stays off, with the note saying so.
[[nodiscard]] Result<Settings> parse_settings(std::string_view toml);

}  // namespace myclean::security
