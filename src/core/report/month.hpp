#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/model/units.hpp"

// The months of the monthly report (reports spec): a local calendar month is the token "YYYY-MM"
// ("2026-11"), the key of its snapshot and its report; and the rule of the monthly job.
namespace myclean::report {

// The monthly job in MyClean's own scheduler (core/sched; never an OS schedule): the 1st of every
// month at 09:00 local time, caught up at the next service start when it was missed.
inline constexpr std::string_view monthly_job = "monthly-report";
inline constexpr std::string_view monthly_schedule = "0 9 1 * *";

// "2026-11" -> November 2026: four digits, '-', two digits 01-12; nullopt for anything else.
[[nodiscard]] std::optional<std::chrono::year_month> parse_month(std::string_view text);
[[nodiscard]] std::string month_text(std::chrono::year_month month);
// The local calendar month `time` falls in.
[[nodiscard]] std::chrono::year_month month_of(WallTime time, const std::chrono::time_zone& zone);

// Whether the monthly job must run at this service start although the scheduler has nothing due:
// it came due this month (`last_run`, the scheduler's last start of the job, is in the current
// local month) and still no snapshot of this month exists, because MyClean stopped during the run
// or the run failed. A month with a snapshot never runs again, so a month gets one report at most.
[[nodiscard]] bool retry_at_start(std::optional<WallTime> last_run, bool month_has_snapshot,
                                  WallTime now, const std::chrono::time_zone& zone);

// The report files of `month` under `reports_dir` (%ProgramData%\MyClean\reports):
// <reports_dir>\YYYY-MM\report.md and <reports_dir>\YYYY-MM\report.html.
[[nodiscard]] std::filesystem::path markdown_file(const std::filesystem::path& reports_dir,
                                                  std::chrono::year_month month);
[[nodiscard]] std::filesystem::path html_file(const std::filesystem::path& reports_dir,
                                              std::chrono::year_month month);

}  // namespace myclean::report
