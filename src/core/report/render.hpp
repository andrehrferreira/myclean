#pragma once

#include <chrono>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

// The monthly report as Markdown and as one self-contained HTML page (reports spec, "Report
// Files"): both are written from the same blocks, so they hold the same content. The HTML loads
// nothing: its style is inline, its chart an inline SVG, and its Content-Security-Policy refuses
// every remote resource.
namespace myclean::report {

struct Rendered {
  std::string markdown;
  std::string html;
};

// The report of `snapshot` (take_snapshot). `diff` is diff(previous, snapshot) against the
// snapshot of month `against`, or null for the first report, which says so and lists the current
// state as the baseline. Times are shown in `zone`.
[[nodiscard]] Rendered render(const nlohmann::json& snapshot, const nlohmann::json& diff,
                              std::optional<std::chrono::year_month> against,
                              const std::chrono::time_zone& zone);

// The changes section alone, as Markdown, for reports.compare.
[[nodiscard]] std::string diff_markdown(const nlohmann::json& diff);

}  // namespace myclean::report
