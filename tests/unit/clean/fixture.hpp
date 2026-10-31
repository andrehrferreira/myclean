#pragma once

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "clean_fakes.hpp"
#include "core/clean/engine.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/packs/loader.hpp"
#include "core/packs/registry.hpp"
#include "core/rules/schema.hpp"
#include "core/safety/deny_list.hpp"

// Shared setup of the engine tests: a machine whose owner is C:\Users\owner, and the rules
// of the knowledge packs shipped in data/packs.
namespace myclean::clean::testing {

inline const WallTime now = std::chrono::sys_days{std::chrono::year{2026} / 10 / 31};

inline WallTime days_ago(int count) {
  return now - std::chrono::days{count};
}

inline safety::ProtectedLocations owner_locations() {
  return {.windows = R"(C:\Windows)",
          .profiles = R"(C:\Users)",
          .owner_profile = R"(C:\Users\owner)",
          .owner_known_folders = {R"(C:\Users\owner\Documents)"},
          .myclean = {R"(C:\ProgramData\MyClean)"}};
}

inline void set_owner_environment(FakeEnvironment& environment) {
  environment.set(L"TEMP", LR"(C:\Users\owner\AppData\Local\Temp)");
  environment.set(L"LOCALAPPDATA", LR"(C:\Users\owner\AppData\Local)");
  environment.set(L"APPDATA", LR"(C:\Users\owner\AppData\Roaming)");
  environment.set(L"USERPROFILE", LR"(C:\Users\owner)");
  environment.set(L"ProgramData", LR"(C:\ProgramData)");
  environment.set(L"ProgramFiles(x86)", LR"(C:\Program Files (x86))");
  environment.set(L"WINDIR", LR"(C:\Windows)");
}

// The clean cases of every shipped pack, as the service loads them (applies_when aside);
// throws when any pack is rejected.
inline const std::vector<rules::CleaningRule>& shipped_rules() {
  static const std::vector<rules::CleaningRule> loaded = [] {
    const packs::LoadReport report =
        packs::load_packs(packs::DiskPackFiles{}, std::filesystem::path{MYCLEAN_DATA_DIR} / "packs",
                          std::nullopt, packs::this_version());
    if (!report.errors.empty()) {
      throw std::runtime_error(report.errors.front().message);
    }
    return packs::make_registry(report.packs).clean;
  }();
  return loaded;
}

inline const rules::CleaningRule& shipped_rule(std::string_view id) {
  const std::vector<rules::CleaningRule>& all = shipped_rules();
  const auto found =
      std::ranges::find_if(all, [id](const rules::CleaningRule& rule) { return rule.id == id; });
  if (found == all.end()) {
    throw std::logic_error("no shipped rule " + std::string(id));
  }
  return *found;
}

inline std::vector<std::filesystem::path> paths_of(const Evaluation& evaluation) {
  std::vector<std::filesystem::path> paths;
  paths.reserve(evaluation.candidates.size());
  for (const Candidate& candidate : evaluation.candidates) {
    paths.push_back(candidate.path);
  }
  std::ranges::sort(paths);
  return paths;
}

inline const Dropped* dropped_at(const Evaluation& evaluation, const std::filesystem::path& path) {
  const auto found = std::ranges::find_if(evaluation.dropped, [&path](const Dropped& dropped) {
    return key_of(dropped.path) == key_of(path);
  });
  return found == evaluation.dropped.end() ? nullptr : &*found;
}

}  // namespace myclean::clean::testing
