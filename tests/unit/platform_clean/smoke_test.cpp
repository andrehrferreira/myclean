#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/clean/engine.hpp"
#include "core/clean/workspaces.hpp"
#include "core/model/result.hpp"
#include "core/rules/data_files.hpp"
#include "core/rules/schema.hpp"
#include "core/safety/deny_list.hpp"
#include "core/safety/paths.hpp"
#include "platform/clean/win32_probes.hpp"
#include "platform/fs/protected_locations.hpp"
#include "platform/git/git_cli.hpp"
#include "shipped_rules.hpp"

// A read-only report of what the shipped catalog would select on this machine. Disabled: it
// reads the owner's real folders and repositories. Nothing is deleted or written. Run with
//   myclean_platform_clean_tests --gtest_also_run_disabled_tests --gtest_filter=*Smoke*
// MYCLEAN_SMOKE_MAX_DEPTH bounds the workspace walk (default: data/workspaces.toml).
namespace myclean::platform {
namespace {

namespace fs = std::filesystem;

double mib(Bytes size) {
  return static_cast<double>(size.count()) / (1024.0 * 1024.0);
}

void report(std::string_view title, const clean::Evaluation& evaluation) {
  struct Totals {
    int items = 0;
    Bytes size;
    std::string tier;
  };
  std::map<std::string, Totals> per_rule;
  for (const clean::Candidate& candidate : evaluation.candidates) {
    Totals& totals = per_rule[candidate.rule_id];
    ++totals.items;
    totals.size += candidate.size;
    totals.tier = std::string(to_string(candidate.tier));
  }
  std::map<std::pair<std::string, std::string>, int> drops;
  for (const clean::Dropped& dropped : evaluation.dropped) {
    ++drops[{dropped.rule_id, dropped.reason}];
  }
  std::cout << std::format("== {}\n", title);
  Bytes total;
  for (const auto& [rule, totals] : per_rule) {
    std::cout << std::format("  {:<40} {:<6} {:>6} items {:>12.1f} MiB\n", rule, totals.tier,
                             totals.items, mib(totals.size));
    total += totals.size;
  }
  std::cout << std::format("  total {:.1f} MiB\n", mib(total));
  for (const auto& [key, count] : drops) {
    std::cout << std::format("  dropped {:<40} {:<28} {}\n", key.first, key.second, count);
  }
  std::vector<const clean::Candidate*> largest;
  for (const clean::Candidate& candidate : evaluation.candidates) {
    if (!candidate.path.empty()) {
      largest.push_back(&candidate);
    }
  }
  std::ranges::sort(largest, std::ranges::greater{},
                    [](const clean::Candidate* candidate) { return candidate->size; });
  largest.resize(std::min<std::size_t>(largest.size(), 15));
  for (const clean::Candidate* candidate : largest) {
    std::cout << std::format("  {:>10.1f} MiB  {}  {}\n", mib(candidate->size), candidate->rule_id,
                             safety::to_utf8(candidate->path));
  }
}

long long seconds_since(std::chrono::steady_clock::time_point started) {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                          started)
      .count();
}

TEST(Smoke, DISABLED_ShippedCatalogOnThisMachine) {
  const std::vector<rules::CleaningRule> shipped = testing::shipped_rules();
  ASSERT_FALSE(shipped.empty());
  const fs::path workspace_file = fs::path{MYCLEAN_DATA_DIR} / "workspaces.toml";
  Result<clean::Workspaces> workspaces =
      rules::DiskDataFiles{}.read(workspace_file).and_then([&](const std::string& text) {
        return clean::parse_workspace_file(text, workspace_file);
      });
  ASSERT_TRUE(workspaces.has_value()) << workspaces.error().message;
  const Win32Environment environment;
  if (const std::optional<std::wstring> depth = environment.variable(L"MYCLEAN_SMOKE_MAX_DEPTH")) {
    workspaces->max_depth = std::stoi(*depth);
  }
  const std::optional<fs::path> git_path = find_git();
  ASSERT_TRUE(git_path.has_value());
  const Win32FileTree tree;
  const Win32Usage usage;
  const GitCli git{*git_path};
  const clean::RuleEngine engine{tree, environment, usage, *protected_locations(nullptr)};
  const WallTime now = std::chrono::system_clock::now();

  auto started = std::chrono::steady_clock::now();
  report("rules", engine.evaluate(shipped, now));
  std::cout << std::format("  ({} s)\n", seconds_since(started));
  started = std::chrono::steady_clock::now();
  report("developer artifacts", engine.scan_artifacts(shipped, *workspaces, git, now));
  std::cout << std::format("  ({} s, max_depth {})\n", seconds_since(started),
                           workspaces->max_depth);
}

}  // namespace
}  // namespace myclean::platform
