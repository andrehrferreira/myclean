#include "core/security/verdicts.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

#include "core/model/result.hpp"
#include "core/model/text.hpp"
#include "core/model/units.hpp"

namespace myclean::security {
namespace {

using Json = nlohmann::json;

constexpr int scan_clean = 0;
constexpr int scan_threats = 2;
constexpr std::size_t max_api_key = 128;

char lower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequal(std::string_view a, std::string_view b) {
  return std::ranges::equal(a, b, [](char x, char y) { return lower(x) == lower(y); });
}

std::string_view trim(std::string_view text) {
  constexpr std::string_view blanks = " \t\r\n";
  const std::size_t first = text.find_first_not_of(blanks);
  if (first == std::string_view::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

std::uint32_t count(const Json& stats, std::string_view key) {
  const auto found = stats.find(key);
  return found != stats.end() && found->is_number_unsigned() ? found->get<std::uint32_t>() : 0;
}

template <typename T>
std::optional<T> number(std::string_view text) {
  T value{};
  const char* const first = std::to_address(text.begin());
  const char* const last = std::to_address(text.end());
  const auto [end, error] = std::from_chars(first, last, value);
  if (text.empty() || error != std::errc{} || end != last) {
    return std::nullopt;
  }
  return value;
}

// The file path of a Defender resource, or empty for one that names no file.
std::string_view resource_file(std::string_view resource) {
  for (const std::string_view prefix :
       {std::string_view("file:_"), std::string_view("containerfile:_")}) {
    if (resource.size() > prefix.size() && iequal(resource.substr(0, prefix.size()), prefix)) {
      const std::string_view file = resource.substr(prefix.size());
      return file.substr(0, file.find("->"));
    }
  }
  return {};
}

bool api_key_text(std::string_view key) {
  return !key.empty() && key.size() <= max_api_key && std::ranges::all_of(key, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  });
}

}  // namespace

Result<HashReport> parse_hash_report(std::string_view json) {
  const Json document = Json::parse(json, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return fail(Errc::invalid_data, "VirusTotal answered with something that is not JSON");
  }
  const Json* stats = nullptr;
  if (const auto data = document.find("data"); data != document.end() && data->is_object()) {
    if (const auto attributes = data->find("attributes");
        attributes != data->end() && attributes->is_object()) {
      if (const auto found = attributes->find("last_analysis_stats");
          found != attributes->end() && found->is_object()) {
        stats = &*found;
      }
    }
  }
  if (stats == nullptr) {
    return fail(Errc::invalid_data, "VirusTotal's answer has no last_analysis_stats");
  }
  const std::uint32_t malicious = count(*stats, "malicious");
  return HashReport{.malicious = malicious,
                    .engines = malicious + count(*stats, "suspicious") +
                               count(*stats, "undetected") + count(*stats, "harmless")};
}

Result<FileScan> parse_file_scan(int exit_code, std::string_view output) {
  if (exit_code == scan_clean) {
    return FileScan{};
  }
  if (exit_code != scan_threats) {
    return fail(Errc::io_error, "MpCmdRun exited with " + std::to_string(exit_code) + ": " +
                                    std::string(trim(output).substr(0, 512)));
  }
  FileScan scan{.detected = true};
  std::size_t start = 0;
  while (start <= output.size()) {
    const std::size_t end = std::min(output.find('\n', start), output.size());
    const std::string_view line = trim(output.substr(start, end - start));
    const std::size_t colon = line.find(':');
    if (colon != std::string_view::npos && trim(line.substr(0, colon)) == "Threat") {
      scan.threat = std::string(trim(line.substr(colon + 1)));
      break;
    }
    start = end + 1;
  }
  return scan;
}

std::optional<WallTime> parse_cim_datetime(std::string_view text) {
  // yyyymmddHHMMSS.mmmmmmsUUU
  constexpr std::size_t length = 25;
  if (text.size() != length || text[14] != '.' || (text[21] != '+' && text[21] != '-')) {
    return std::nullopt;
  }
  const auto year = number<int>(text.substr(0, 4));
  const auto month = number<unsigned>(text.substr(4, 2));
  const auto day = number<unsigned>(text.substr(6, 2));
  const auto hour = number<int>(text.substr(8, 2));
  const auto minute = number<int>(text.substr(10, 2));
  const auto second = number<int>(text.substr(12, 2));
  const auto micros = number<int>(text.substr(15, 6));
  const auto offset = number<int>(text.substr(22, 3));
  if (!year || !month || !day || !hour || !minute || !second || !micros || !offset) {
    return std::nullopt;
  }
  const std::chrono::year_month_day date{std::chrono::year{*year}, std::chrono::month{*month},
                                         std::chrono::day{*day}};
  if (!date.ok() || *hour > 23 || *minute > 59 || *second > 60) {
    return std::nullopt;
  }
  const std::chrono::minutes utc_offset{text[21] == '+' ? *offset : -*offset};
  const auto local = std::chrono::sys_days{date} + std::chrono::hours{*hour} +
                     std::chrono::minutes{*minute} + std::chrono::seconds{*second} +
                     std::chrono::microseconds{*micros};
  return std::chrono::time_point_cast<WallTime::duration>(local - utc_offset);
}

const Detection* detection_of(std::span<const Detection> detections,
                              const std::filesystem::path& image) {
  const std::string path = utf8_text(image);
  const Detection* newest = nullptr;
  for (const Detection& detection : detections) {
    const bool names = std::ranges::any_of(detection.resources, [&](const std::string& resource) {
      const std::string_view file = resource_file(resource);
      return !file.empty() && iequal(file, path);
    });
    if (names && (newest == nullptr || detection.time > newest->time)) {
      newest = &detection;
    }
  }
  return newest;
}

std::string hex(std::span<const std::uint8_t> digest) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string text;
  text.reserve(digest.size() * 2);
  for (const std::uint8_t byte : digest) {
    text.push_back(digits[byte >> 4U]);
    text.push_back(digits[byte & 0x0FU]);
  }
  return text;
}

Result<Settings> parse_settings(std::string_view toml) {
  toml::table root;
  try {
    root = toml::parse(toml, std::string_view{"security.toml"});
  } catch (const toml::parse_error& error) {
    return fail(Errc::invalid_data, "security.toml: " + std::string(error.description()));
  }
  Settings settings;
  const toml::node_view<toml::node> reverse_dns = root["network"]["reverse_dns"];
  const toml::node_view<toml::node> enabled = root["virustotal"]["enabled"];
  const toml::node_view<toml::node> key = root["virustotal"]["api_key"];
  if ((reverse_dns && !reverse_dns.is_boolean()) || (enabled && !enabled.is_boolean()) ||
      (key && !key.is_string())) {
    return fail(Errc::invalid_data,
                "security.toml: reverse_dns and enabled must be booleans, api_key a string");
  }
  settings.reverse_dns = reverse_dns.value_or(false);
  const bool wanted = enabled.value_or(false);
  const std::string api_key(trim(key.value_or(std::string_view{})));
  if (wanted && api_key.empty()) {
    settings.virustotal_note =
        "VirusTotal stays off: it needs the owner's API key ([virustotal] api_key in "
        "%ProgramData%\\MyClean\\security.toml)";
  } else if (wanted && !api_key_text(api_key)) {
    settings.virustotal_note =
        "VirusTotal stays off: the API key must be letters and digits only (at most 128)";
  } else if (wanted) {
    settings.virustotal = true;
    settings.api_key = api_key;
  }
  return settings;
}

}  // namespace myclean::security
