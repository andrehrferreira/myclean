#include "core/report/render.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/report/month.hpp"
#include "core/report/snapshot.hpp"
#include "core/store/database.hpp"

namespace myclean::report {
namespace {

using Json = nlohmann::json;
using Row = std::vector<std::string>;

constexpr std::size_t top_files = 10;

// One piece of the report. Text is plain; each writer escapes it.
struct Block {
  enum class Kind { heading, paragraph, bullets, table, chart };
  Kind kind = Kind::paragraph;
  int level = 2;
  std::string text;                // heading, paragraph, chart title
  std::vector<std::string> lines;  // bullets
  std::vector<Row> rows;           // table and chart: the first row is the header
  std::vector<double> values;      // chart: one value per row after the header
};

class Page {
 public:
  void heading(int level, std::string text) {
    blocks_.push_back({.kind = Block::Kind::heading, .level = level, .text = std::move(text)});
  }
  void paragraph(std::string text) {
    blocks_.push_back({.kind = Block::Kind::paragraph, .text = std::move(text)});
  }
  void bullets(std::vector<std::string> lines) {
    blocks_.push_back({.kind = Block::Kind::bullets, .lines = std::move(lines)});
  }
  void table(std::vector<Row> rows) {
    blocks_.push_back({.kind = Block::Kind::table, .rows = std::move(rows)});
  }
  void chart(std::string title, std::vector<Row> rows, std::vector<double> values) {
    blocks_.push_back({.kind = Block::Kind::chart,
                       .text = std::move(title),
                       .rows = std::move(rows),
                       .values = std::move(values)});
  }
  [[nodiscard]] const std::vector<Block>& blocks() const { return blocks_; }

 private:
  std::vector<Block> blocks_;
};

// ---- values ------------------------------------------------------------------------------------

std::string text(const Json& object, std::string_view key) {
  const auto found = object.find(key);
  if (found == object.end() || found->is_null()) {
    return {};
  }
  return found->is_string() ? found->get<std::string>()
                            : found->dump(-1, ' ', false, Json::error_handler_t::replace);
}

double number(const Json& object, std::string_view key) {
  const auto found = object.find(key);
  return found != object.end() && found->is_number() ? found->get<double>() : 0.0;
}

std::string bytes(double count) {
  constexpr std::array<std::string_view, 5> units{"B", "KiB", "MiB", "GiB", "TiB"};
  double value = std::fabs(count);
  std::size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < units.size()) {
    value /= 1024.0;
    ++unit;
  }
  const std::string sign = count < 0 ? "-" : "";
  return unit == 0 ? std::format("{}{:.0f} {}", sign, value, units.at(unit))
                   : std::format("{}{:.1f} {}", sign, value, units.at(unit));
}

std::string signed_bytes(double count) {
  return (count > 0 ? "+" : "") + bytes(count);
}

std::string local_time(std::int64_t seconds, const std::chrono::time_zone& zone) {
  const auto local = zone.to_local(std::chrono::sys_seconds{std::chrono::seconds{seconds}});
  return std::format("{:%Y-%m-%d %H:%M}", local);
}

std::string percent(double value) {
  return std::format("{:.1f} %", value);
}

std::string or_unknown(const std::string& value) {
  return value.empty() ? "unknown" : value;
}

std::string not_collected(const Json& category) {
  return "Not collected: " + or_unknown(text(category, "reason")) + ".";
}

// ---- items -------------------------------------------------------------------------------------

std::string item_text(std::string_view name, const Json& item) {
  if (name == "startup_items") {
    return text(item, "name") + " (" + text(item, "source") + ", " + text(item, "scope") +
           "): " + text(item, "command");
  }
  if (name == "browser_extensions") {
    std::string line = text(item, "name") + " (" + text(item, "id") + ") in " +
                       text(item, "browser") + "/" + text(item, "profile") + ", from " +
                       text(item, "origin");
    if (item.contains("flag") && item.at("flag").is_string()) {
      line += ", " + item.at("flag").get<std::string>();
    }
    return line;
  }
  if (name == "services") {
    return text(item, "display_name") + " (" + text(item, "name") + "): " + text(item, "command");
  }
  if (name == "listening_ports") {
    std::string protocol = text(item, "protocol");
    std::ranges::transform(protocol, protocol.begin(), [](char c) {
      return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    });
    return protocol + " " + text(item, "port") + " by " + or_unknown(text(item, "image")) +
           " (pid " + text(item, "pid") + ")";
  }
  std::string seen;
  for (const Json& kind : item.value("seen_as", Json::array())) {
    seen += (seen.empty() ? "" : ", ") + kind.get<std::string>();
  }
  return text(item, "path") + " (" + text(item, "status") + "; seen as " + seen + ")";
}

std::string_view title_of(std::string_view name) {
  if (name == "startup_items") {
    return "Startup items";
  }
  if (name == "browser_extensions") {
    return "Browser extensions";
  }
  if (name == "services") {
    return "Services";
  }
  if (name == "listening_ports") {
    return "Listening ports";
  }
  return "Unsigned binaries";
}

// ---- the changes -------------------------------------------------------------------------------

void set_changes(Page& page, std::string_view name, const Json& change) {
  page.heading(3, std::string(title_of(name)));
  if (change.contains("not_compared")) {
    page.paragraph("Not compared: " + text(change, "not_compared") + ".");
    return;
  }
  std::vector<std::string> lines;
  for (const Json& item : change.value("added", Json::array())) {
    lines.push_back("New: " + item_text(name, item));
  }
  for (const Json& item : change.value("removed", Json::array())) {
    lines.push_back("Gone: " + item_text(name, item));
  }
  if (lines.empty()) {
    page.paragraph("No change.");
  } else {
    page.bullets(std::move(lines));
  }
}

void folder_changes(Page& page, const Json& change) {
  page.heading(3, "Top folders");
  if (change.contains("not_compared")) {
    page.paragraph("Not compared: " + text(change, "not_compared") + ".");
    return;
  }
  std::vector<Row> rows{{"Folder", "Before", "Now", "Growth"}};
  for (const Json& folder : change.value("grown", Json::array())) {
    rows.push_back({text(folder, "path"), bytes(number(folder, "before")),
                    bytes(number(folder, "after")), signed_bytes(number(folder, "change"))});
  }
  if (rows.size() > 1) {
    page.table(std::move(rows));
  } else {
    page.paragraph("No top folder grew.");
  }
  std::vector<std::string> lines;
  for (const Json& folder : change.value("new", Json::array())) {
    lines.push_back("New in the top list: " + text(folder, "path") + ", " +
                    bytes(number(folder, "after")));
  }
  for (const Json& folder : change.value("gone", Json::array())) {
    lines.push_back("Left the top list: " + text(folder, "path") + ", was " +
                    bytes(number(folder, "before")));
  }
  if (!lines.empty()) {
    page.bullets(std::move(lines));
  }
}

void drive_changes(Page& page, const Json& change) {
  page.heading(3, "Disk health");
  if (change.is_object() && change.contains("not_compared")) {
    page.paragraph("Not compared: " + text(change, "not_compared") + ".");
    return;
  }
  std::vector<std::string> lines;
  for (const Json& drive : change) {
    const std::string before = or_unknown(text(drive, "status_before"));
    const std::string after = text(drive, "status_after");
    std::string line = or_unknown(text(drive, "model"));
    line += ": ";
    if (after.empty()) {
      line += "gone (was " + before + ")";
    } else if (before == after) {
      line += after + ", unchanged";
    } else {
      line += before;
      line += " -> ";
      line += after;
    }
    const Json counters = drive.value("changes", Json::object());
    for (const auto& [counter, values] : counters.items()) {
      line += "; " + counter + " " + or_unknown(text(values, "before")) + " -> " +
              or_unknown(text(values, "after"));
    }
    lines.push_back(std::move(line));
  }
  if (lines.empty()) {
    page.paragraph("No drive was read.");
  } else {
    page.bullets(std::move(lines));
  }
}

void free_space_change(Page& page, const Json& change) {
  page.heading(3, "Free space");
  if (change.contains("not_compared")) {
    page.paragraph("Not compared: " + text(change, "not_compared") + ".");
    return;
  }
  page.paragraph(text(change, "volume") + " had " + bytes(number(change, "before")) +
                 " free, now " + bytes(number(change, "after")) + " (" +
                 signed_bytes(number(change, "change")) + ").");
}

std::string count_text(const Json& value, bool size) {
  if (!value.is_number()) {
    return "not collected";
  }
  return size ? bytes(value.get<double>()) : std::to_string(value.get<std::int64_t>());
}

void count_changes(Page& page, const Json& counts, const std::string& from, const std::string& to) {
  page.heading(3, "Monthly counts");
  std::vector<Row> rows{{"Count", from, to}};
  const auto add = [&](std::string_view label, std::string_view key, bool size) {
    const Json value = counts.value(key, Json::object());
    rows.push_back({std::string(label), count_text(value.value("before", Json()), size),
                    count_text(value.value("after", Json()), size)});
  };
  add("Governor interventions", "governor_interventions", false);
  add("Freeze incidents", "freeze_incidents", false);
  add("Space freed by automatic cleaning", "space_freed_bytes", true);
  page.table(std::move(rows));
}

void changes(Page& page, const Json& diff) {
  const Json sets = diff.value("sets", Json::object());
  for (const std::string_view name : set_categories) {
    set_changes(page, name, sets.value(name, Json::object()));
  }
  folder_changes(page, diff.value("top_folders", Json::object()));
  drive_changes(page, diff.value("disk_health", Json::array()));
  free_space_change(page, diff.value("free_space", Json::object()));
  count_changes(page, diff.value("counts", Json::object()), text(diff, "from"), text(diff, "to"));
}

// ---- the sections of the current state ---------------------------------------------------------

void health_section(Page& page, const Json& health) {
  page.heading(2, "Health (30-day averages)");
  if (!collected(health)) {
    page.paragraph(not_collected(health));
    return;
  }
  const Json cpu = health.value("cpu", Json());
  const Json memory = health.value("memory_available", Json());
  page.paragraph((cpu.is_object() ? "CPU averaged " + percent(number(cpu, "mean")) +
                                        ", peaking at " + percent(number(cpu, "max")) + ". "
                                  : "No CPU history yet. ") +
                 (memory.is_object()
                      ? "Available memory averaged " + bytes(number(memory, "mean")) + ", lowest " +
                            bytes(number(memory, "min")) + "."
                      : "No memory history yet."));
  std::vector<Row> rows{{"Program", "Hours running", "Average CPU", "Average RAM", "Peak RAM"}};
  for (const Json& program : health.value("programs", Json::array())) {
    rows.push_back({text(program, "image"), std::format("{:.0f}", number(program, "hours_running")),
                    percent(number(program, "cpu_percent")), bytes(number(program, "ram_average")),
                    bytes(number(program, "ram_peak"))});
  }
  if (rows.size() > 1) {
    page.paragraph(
        "Programs that ran at least 24 hours of the last 30 days, the most CPU first "
        "(CPU as a share of all processors while they ran):");
    page.table(std::move(rows));
  } else {
    page.paragraph("No program ran 24 hours or more in the last 30 days.");
  }
}

void storage_section(Page& page, const Json& folders, const Json& candidates) {
  page.heading(2, "Storage");
  if (collected(folders)) {
    std::vector<Row> rows{{"Folder", "Size on disk", "Files"}};
    for (const Json& folder : folders.value("folders", Json::array())) {
      rows.push_back(
          {text(folder, "path"), bytes(number(folder, "size_on_disk")), text(folder, "files")});
    }
    page.paragraph("The largest folders of the system volume, " + text(folders, "volume") +
                   ", by size on disk:");
    page.table(std::move(rows));
    std::vector<Row> files{{"File", "Size on disk"}};
    for (const Json& file : folders.value("files", Json::array())) {
      if (files.size() > top_files) {
        break;
      }
      files.push_back({text(file, "path"), bytes(number(file, "size_on_disk"))});
    }
    page.table(std::move(files));
  } else {
    page.paragraph("Top folders: " + not_collected(folders));
  }
  if (!collected(candidates)) {
    page.paragraph("Cleanup candidates: " + not_collected(candidates));
    return;
  }
  std::vector<std::string> lines;
  const auto add = [&](std::string_view label, std::string_view key) {
    const Json found = candidates.value(key, Json::object());
    lines.push_back(found.contains("error")
                        ? std::string(label) + ": not collected: " + text(found, "error")
                        : std::string(label) + ": " + text(found, "count") +
                              (found.value("count", 0) == 1 ? " file, " : " files, ") +
                              bytes(number(found, "bytes")));
  };
  add("Old downloads", "old_downloads");
  add("Duplicate photos that could go", "duplicate_photos");
  add("Installers of installed programs", "used_installers");
  page.paragraph(
      "Cleanup candidates, listed only (nothing was deleted; propose a cleanup and "
      "approve it in the MyClean tray to act on them):");
  page.bullets(std::move(lines));
}

void free_space_section(Page& page, const Json& free) {
  page.heading(2, "Free space trend");
  if (!collected(free)) {
    page.paragraph(not_collected(free));
    return;
  }
  page.paragraph(text(free, "volume") + " has " + bytes(number(free, "free")) + " free of " +
                 bytes(number(free, "size")) +
                 "; over the last 30 days its free space changed by " +
                 signed_bytes(number(free, "change")) + ".");
  std::vector<Row> rows{{"Day", "Free"}};
  std::vector<double> values;
  for (const Json& point : free.value("points", Json::array())) {
    const auto day = std::chrono::sys_seconds{
        std::chrono::seconds{static_cast<std::int64_t>(number(point, "time"))}};
    rows.push_back({std::format("{:%Y-%m-%d}", day), bytes(number(point, "free"))});
    values.push_back(number(point, "free"));
  }
  page.chart("Free space on " + text(free, "volume") + " by day:", std::move(rows),
             std::move(values));
}

void security_section(Page& page, const Json& security) {
  page.heading(2, "Security");
  if (!collected(security)) {
    page.paragraph(not_collected(security));
    return;
  }
  const Json counts = security.value("counts", Json::object());
  page.paragraph("Persistence points: " + text(counts, "default") + " Windows defaults, " +
                 text(counts, "ok") + " ok, " + text(counts, "suspicious") + " suspicious, " +
                 text(counts, "known_dangerous") +
                 " known dangerous (a verdict of Microsoft Defender or VirusTotal). Defender "
                 "detections in its history: " +
                 text(security, "defender_detections") +
                 ". Browser extensions probably not "
                 "installed by you: " +
                 or_unknown(text(security, "flagged_extensions")) + ".");
  std::vector<std::string> lines;
  for (const Json& item : security.value("known_dangerous", Json::array())) {
    lines.push_back(text(item, "name") + " (" + text(item, "source") +
                    "): " + text(item, "summary"));
  }
  for (const Json& item : security.value("suspicious", Json::array())) {
    lines.push_back(text(item, "name") + " (" + text(item, "source") +
                    "): " + text(item, "summary"));
  }
  if (!lines.empty()) {
    page.bullets(std::move(lines));
  }
  const Json talkers = security.value("background_talkers", Json());
  if (talkers.is_array() && !talkers.empty()) {
    std::vector<Row> rows{{"Process without a window", "Sent", "Received"}};
    for (const Json& talker : talkers) {
      rows.push_back({text(talker, "image"), bytes(number(talker, "sent")),
                      bytes(number(talker, "received"))});
    }
    page.paragraph("The busiest network talkers of the last 24 hours that showed no window:");
    page.table(std::move(rows));
  }
}

void counts_sections(Page& page, const Json& governor, const Json& freezes, const Json& freed) {
  page.heading(2, "Governor");
  if (collected(governor)) {
    std::vector<std::string> lines;
    const Json steps = governor.value("by_step", Json::object());
    for (const auto& [step, count] : steps.items()) {
      lines.push_back(step + ": " + count.dump());
    }
    page.paragraph(text(governor, "interventions") +
                   " interventions in the period: " + text(governor, "applied") + " applied, " +
                   text(governor, "recorded_only") + " recorded only (observe mode).");
    if (!lines.empty()) {
      page.bullets(std::move(lines));
    }
  } else {
    page.paragraph(not_collected(governor));
  }
  page.heading(2, "Freezes");
  page.paragraph(collected(freezes)
                     ? text(freezes, "incidents") + " copy/paste freeze incidents in the period" +
                           (freezes.value("longest_ms", Json()).is_number()
                                ? ", the longest " + text(freezes, "longest_ms") + " ms."
                                : ".")
                     : not_collected(freezes));
  page.heading(2, "Space freed by automatic cleaning");
  page.paragraph(collected(freed) ? bytes(number(freed, "bytes")) + " in " + text(freed, "items") +
                                        " items during the period."
                                  : not_collected(freed));
}

void disk_section(Page& page, const Json& disks) {
  page.heading(2, "Disk health");
  if (!collected(disks)) {
    page.paragraph(not_collected(disks));
    return;
  }
  std::vector<Row> rows{
      {"Drive", "Status", "Temperature", "Used", "Spare", "Media errors", "Power-on hours"}};
  for (const Json& drive : disks.value("drives", Json::array())) {
    std::string status = text(drive, "status");
    for (const Json& reason : drive.value("reasons", Json::array())) {
      status += "; " + reason.get<std::string>();
    }
    rows.push_back(
        {text(drive, "model"), status, or_unknown(text(drive, "temperature_c")),
         or_unknown(text(drive, "percent_used")), or_unknown(text(drive, "available_spare")),
         or_unknown(text(drive, "media_errors")), or_unknown(text(drive, "power_on_hours"))});
  }
  page.table(std::move(rows));
}

void inventory(Page& page, const Json& snapshot, bool baseline) {
  page.heading(2, baseline ? "Baseline: the current state" : "Current state");
  for (const std::string_view name : set_categories) {
    page.heading(3, std::string(title_of(name)));
    const Json found = category(snapshot, name);
    if (!collected(found)) {
      page.paragraph(not_collected(found));
      continue;
    }
    std::vector<std::string> lines;
    for (const Json& item : found.value("items", Json::array())) {
      lines.push_back(item_text(name, item));
    }
    if (lines.empty()) {
      page.paragraph("None.");
    } else {
      page.bullets(std::move(lines));
    }
  }
}

// ---- writers -----------------------------------------------------------------------------------

bool word_character(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool punctuation(char c) {
  return c > ' ' && c < 0x7F && !word_character(c);
}

// Plain text as Markdown that shows exactly that text, escaping only what Markdown would read as
// markup: '*', '`', '[', ']', '<' and '|' (tables) always, '_' at a word boundary (inside a word
// it never emphasizes) and '\' before punctuation, so a path such as C:\Users stays readable.
std::string markdown_escape(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char c = value[index];
    const char before = index > 0 ? value[index - 1] : ' ';
    const char after = index + 1 < value.size() ? value[index + 1] : ' ';
    const bool escape = std::string_view("*`[]<|").find(c) != std::string_view::npos ||
                        (c == '_' && !(word_character(before) && word_character(after))) ||
                        (c == '\\' && (punctuation(after) || index + 1 == value.size()));
    if (escape) {
      out.push_back('\\');
    }
    out.push_back(c == '\n' || c == '\r' ? ' ' : c);
  }
  return out;
}

std::string markdown_table(const std::vector<Row>& rows) {
  std::string out;
  for (std::size_t index = 0; index < rows.size(); ++index) {
    out += "|";
    for (const std::string& cell : rows[index]) {
      out += " " + markdown_escape(cell) + " |";
    }
    out += "\n";
    if (index == 0) {
      out += "|";
      for (std::size_t column = 0; column < rows[0].size(); ++column) {
        out += " --- |";
      }
      out += "\n";
    }
  }
  return out + "\n";
}

std::string markdown(const Page& page) {
  std::string out;
  for (const Block& block : page.blocks()) {
    switch (block.kind) {
      case Block::Kind::heading:
        out += std::string(static_cast<std::size_t>(block.level), '#') + " " +
               markdown_escape(block.text) + "\n\n";
        break;
      case Block::Kind::paragraph:
        out += markdown_escape(block.text) + "\n\n";
        break;
      case Block::Kind::bullets:
        for (const std::string& line : block.lines) {
          out += "- " + markdown_escape(line) + "\n";
        }
        out += "\n";
        break;
      case Block::Kind::chart:
        out += markdown_escape(block.text) + "\n\n" + markdown_table(block.rows);
        break;
      case Block::Kind::table:
        out += markdown_table(block.rows);
        break;
    }
  }
  return out;
}

std::string html_escape(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&#39;";
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

std::string html_table(const std::vector<Row>& rows) {
  std::string out = "<table>\n";
  for (std::size_t index = 0; index < rows.size(); ++index) {
    const std::string_view cell_tag = index == 0 ? "th" : "td";
    out += "<tr>";
    for (const std::string& cell : rows[index]) {
      out += std::format("<{0}>{1}</{0}>", cell_tag, html_escape(cell));
    }
    out += "</tr>\n";
  }
  return out + "</table>\n";
}

// An inline SVG line of `values`, oldest left, scaled between their lowest and highest value.
std::string html_chart(const Block& block) {
  constexpr double width = 600.0;
  constexpr double height = 160.0;
  constexpr double margin = 8.0;
  std::string points;
  if (!block.values.empty()) {
    const auto [low, high] = std::ranges::minmax(block.values);
    const double span = high > low ? high - low : 1.0;
    const double step = block.values.size() > 1
                            ? (width - (2 * margin)) / static_cast<double>(block.values.size() - 1)
                            : 0.0;
    for (std::size_t index = 0; index < block.values.size(); ++index) {
      const double x = margin + (step * static_cast<double>(index));
      const double y =
          height - margin - ((block.values[index] - low) / span * (height - (2 * margin)));
      points += std::format("{}{:.1f},{:.1f}", points.empty() ? "" : " ", x, y);
    }
  }
  return std::format(
             "<figure><figcaption>{0}</figcaption><svg viewBox=\"0 0 600 160\" role=\"img\" "
             "aria-label=\"{0}\"><polyline fill=\"none\" stroke=\"currentColor\" "
             "stroke-width=\"2\" points=\"{1}\"/></svg></figure>\n",
             html_escape(block.text), points) +
         html_table(block.rows);
}

constexpr std::string_view html_style = R"css(
:root { color-scheme: light dark; --text: #1d1d1f; --muted: #5f6368; --line: #d0d4d9;
  --head: #f1f3f5; --background: #ffffff; --accent: #2c6bed; }
@media (prefers-color-scheme: dark) {
  :root { --text: #e8eaed; --muted: #9aa0a6; --line: #3c4043; --head: #202124;
    --background: #171717; --accent: #8ab4f8; } }
body { margin: 0 auto; padding: 16px; max-width: 980px; background: var(--background);
  color: var(--text); font: 15px/1.5 "Segoe UI", system-ui, sans-serif; }
h1 { font-size: 26px; } h2 { font-size: 20px; border-bottom: 1px solid var(--line);
  padding-bottom: 4px; margin-top: 32px; } h3 { font-size: 16px; margin-top: 20px; }
table { border-collapse: collapse; width: 100%; margin: 8px 0 16px; font-size: 14px;
  display: block; overflow-x: auto; }
th, td { border: 1px solid var(--line); padding: 4px 8px; text-align: left;
  vertical-align: top; overflow-wrap: anywhere; }
th { background: var(--head); }
li { overflow-wrap: anywhere; }
figure { margin: 8px 0; } figcaption { color: var(--muted); }
svg { width: 100%; height: auto; color: var(--accent); background: var(--head); }
)css";

std::string html(const Page& page, std::string_view title) {
  std::string out =
      "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
      "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
      "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; "
      "style-src 'unsafe-inline'\">\n<title>" +
      html_escape(title) + "</title>\n<style>" + std::string(html_style) +
      "</style>\n</head>\n<body>\n";
  for (const Block& block : page.blocks()) {
    switch (block.kind) {
      case Block::Kind::heading:
        out += std::format("<h{0}>{1}</h{0}>\n", block.level, html_escape(block.text));
        break;
      case Block::Kind::paragraph:
        out += "<p>" + html_escape(block.text) + "</p>\n";
        break;
      case Block::Kind::bullets:
        out += "<ul>\n";
        for (const std::string& line : block.lines) {
          out += "<li>" + html_escape(line) + "</li>\n";
        }
        out += "</ul>\n";
        break;
      case Block::Kind::chart:
        out += html_chart(block);
        break;
      case Block::Kind::table:
        out += html_table(block.rows);
        break;
    }
  }
  return out + "</body>\n</html>\n";
}

// The paragraph under "Changes since": which snapshot, and the months without one in between.
std::string compared_with(std::chrono::year_month against, std::chrono::year_month month) {
  std::string line = "Compared with the snapshot of " + month_text(against) + ".";
  const std::chrono::year_month first_missing = against + std::chrono::months{1};
  if (first_missing < month) {
    const std::chrono::year_month last_missing = month - std::chrono::months{1};
    line += first_missing == last_missing ? " No snapshot exists for " + month_text(first_missing)
                                          : " No snapshot exists for " + month_text(first_missing) +
                                                " to " + month_text(last_missing);
    line += ", so these changes cover more than one month.";
  }
  return line;
}

}  // namespace

Rendered render(const Json& snapshot, const Json& diff,
                std::optional<std::chrono::year_month> against,
                const std::chrono::time_zone& zone) {
  const std::string month = snapshot.value("month", std::string());
  const Json period = snapshot.value("period", Json::object());
  Page page;
  const std::string title = "MyClean monthly report: " + month;
  page.heading(1, title);
  page.paragraph("Taken " +
                 local_time(static_cast<std::int64_t>(number(snapshot, "taken_at")), zone) +
                 " local time; the counts cover " +
                 local_time(static_cast<std::int64_t>(number(period, "from")), zone) + " to " +
                 local_time(static_cast<std::int64_t>(number(period, "to")), zone) +
                 ". Making this report changed nothing on this PC: what it finds is listed only, "
                 "and acting on it takes a plan you approve in the MyClean tray.");
  const std::optional<std::chrono::year_month> current = parse_month(month);
  if (!against || diff.is_null() || !current) {
    page.heading(2, "First report");
    page.paragraph(
        "First report, no previous snapshot to compare. The current state below is "
        "the baseline the next report compares with.");
  } else {
    page.heading(2, "Changes since " + month_text(*against));
    page.paragraph(compared_with(*against, *current));
    changes(page, diff);
  }
  health_section(page, category(snapshot, "health"));
  storage_section(page, category(snapshot, "top_folders"),
                  category(snapshot, "cleanup_candidates"));
  free_space_section(page, category(snapshot, "free_space"));
  security_section(page, category(snapshot, "security"));
  counts_sections(page, category(snapshot, "governor"), category(snapshot, "freezes"),
                  category(snapshot, "space_freed"));
  disk_section(page, category(snapshot, "disk_health"));
  inventory(page, snapshot, !against || diff.is_null());
  return Rendered{.markdown = markdown(page), .html = html(page, title)};
}

std::string diff_markdown(const Json& diff) {
  Page page;
  page.heading(2, "Changes from " + text(diff, "from") + " to " + text(diff, "to"));
  changes(page, diff);
  return markdown(page);
}

}  // namespace myclean::report
