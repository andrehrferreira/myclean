#include "fixture_runner.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

#include "clean_fakes.hpp"
#include "core/clean/engine.hpp"
#include "core/clean/patterns.hpp"
#include "core/clean/walk.hpp"
#include "core/clean/workspaces.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/packs/fields.hpp"
#include "core/packs/schema.hpp"
#include "core/rules/data_files.hpp"
#include "core/rules/schema.hpp"
#include "core/safety/deny_list.hpp"

namespace myclean::packs::testing {

namespace {

namespace fs = std::filesystem;
using clean::testing::FakeEnvironment;
using clean::testing::FakeGit;
using clean::testing::FakeTree;
using clean::testing::FakeUsage;
using clean::testing::key_of;
using detail::Fields;
using detail::from_utf8;
using detail::utf8;

const WallTime now = std::chrono::sys_days{std::chrono::year{2026} / 10 / 31};

constexpr std::array<std::string_view, 4> kind_names = {"clean", "attribution", "relocate",
                                                        "compress"};
enum class Kind { clean, attribution, relocate, compress };

// A Windows 11 PC whose owner is C:\Users\owner: the reference machine of every fixture.
void set_reference_environment(FakeEnvironment& environment) {
  environment.set(L"SystemDrive", L"C:");
  environment.set(L"SystemRoot", LR"(C:\Windows)");
  environment.set(L"WINDIR", LR"(C:\Windows)");
  environment.set(L"ProgramData", LR"(C:\ProgramData)");
  environment.set(L"ProgramFiles", LR"(C:\Program Files)");
  environment.set(L"ProgramFiles(x86)", LR"(C:\Program Files (x86))");
  environment.set(L"PUBLIC", LR"(C:\Users\Public)");
  environment.set(L"USERPROFILE", LR"(C:\Users\owner)");
  environment.set(L"APPDATA", LR"(C:\Users\owner\AppData\Roaming)");
  environment.set(L"LOCALAPPDATA", LR"(C:\Users\owner\AppData\Local)");
  environment.set(L"TEMP", LR"(C:\Users\owner\AppData\Local\Temp)");
  environment.set(L"TMP", LR"(C:\Users\owner\AppData\Local\Temp)");
}

safety::ProtectedLocations reference_locations() {
  return {.windows = R"(C:\Windows)",
          .profiles = R"(C:\Users)",
          .owner_profile = R"(C:\Users\owner)",
          .owner_known_folders = {R"(C:\Users\owner\Documents)", R"(C:\Users\owner\Desktop)",
                                  R"(C:\Users\owner\Pictures)", R"(C:\Users\owner\Videos)",
                                  R"(C:\Users\owner\Music)"},
          .myclean = {R"(C:\ProgramData\MyClean)", R"(C:\Program Files\MyClean)"}};
}

// The fake machine a fixture describes.
struct Machine {
  FakeTree tree;
  FakeEnvironment environment;
  FakeUsage usage;
  FakeGit git;
  clean::Workspaces workspaces{.roots = {}, .skip = {L".git"}, .max_depth = 8};
};

void read_machine(const Fields& root, Machine& machine) {
  set_reference_environment(machine.environment);
  if (const toml::table* env = root.table("env")) {
    const Fields fields(*env, root.where() + ": [env]");
    for (const auto& [name, value] : *env) {
      const std::string text = fields.required_string(name.str());
      machine.environment.set(from_utf8(name.str()).native(), from_utf8(text).native());
    }
  }
  if (const toml::table* workspace = root.table("workspace")) {
    const Fields fields(*workspace, root.where() + ": [workspace]");
    fields.only({"roots"});
    for (const std::string& path : fields.required_strings("roots")) {
      machine.workspaces.roots.push_back(from_utf8(path));
    }
  }
  int number = 0;
  for (const toml::table* table : root.tables("entry")) {
    const Fields fields(*table, root.where() + ": entry #" + std::to_string(++number));
    fields.only({"path", "size", "age_days", "folder", "git_ignored"});
    const fs::path path = from_utf8(fields.required_string("path"));
    if (!path.has_root_name() || !path.has_root_directory()) {
      fields.fail("path", "must be an absolute path");
    }
    const WallTime touched =
        now - std::chrono::days{fields.integer("age_days", 0, 36500).value_or(365)};
    try {
      if (fields.boolean("folder").value_or(false)) {
        machine.tree.add_directory(path, touched);
      } else {
        const std::int64_t size = fields.integer("size", 0, std::int64_t{1} << 50).value_or(4096);
        machine.tree.add_file(path, Bytes{static_cast<std::uint64_t>(size)}, touched);
      }
    } catch (const std::logic_error&) {
      fields.fail("path", "repeats an entry already in the tree (or one of its folders)");
    }
    if (fields.boolean("git_ignored").value_or(false)) {
      machine.git.ignore(path);
    }
  }
}

// The '\'-separated components of `text` (std::filesystem does not take "?:" for a drive).
std::vector<std::wstring_view> components(std::wstring_view text) {
  std::vector<std::wstring_view> parts;
  for (std::size_t start = 0; start <= text.size();) {
    const std::size_t end = std::min(text.find(L'\\', start), text.size());
    parts.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return parts;
}

// True when one of `patterns` (with %VARIABLES% expanded) names `path`, component by component.
bool names(const std::vector<std::string>& patterns, const fs::path& path,
           const clean::IEnvironment& environment) {
  return std::ranges::any_of(patterns, [&](const std::string& pattern) {
    const std::optional<std::wstring> expanded =
        clean::expand_variables(from_utf8(pattern).native(), environment);
    return expanded && std::ranges::equal(components(path.native()), components(*expanded),
                                          [](std::wstring_view name, std::wstring_view part) {
                                            return clean::wildcard_match(name, part);
                                          });
  });
}

std::vector<fs::path> resolved(const std::vector<std::string>& patterns, const Machine& machine) {
  std::vector<fs::path> paths;
  for (const std::string& pattern : patterns) {
    const std::optional<std::wstring> expanded =
        clean::expand_variables(from_utf8(pattern).native(), machine.environment);
    if (!expanded) {
      continue;
    }
    for (const clean::Entry& entry : clean::resolve(machine.tree, fs::path{*expanded}).entries) {
      paths.push_back(entry.path);
    }
  }
  return paths;
}

using Selects = std::function<bool(const fs::path&)>;

Selects contains(const std::vector<fs::path>& paths) {
  return [keys = [&paths] {
    std::vector<std::wstring> folded;
    std::ranges::transform(paths, std::back_inserter(folded), key_of);
    return folded;
  }()](const fs::path& path) { return std::ranges::find(keys, key_of(path)) != keys.end(); };
}

Selects clean_selection(const Pack& pack, const rules::CleaningRule& rule, const Machine& machine) {
  if (rule.strategy == rules::Strategy::native || rule.strategy == rules::Strategy::handler) {
    return contains(resolved(rule.targets, machine));
  }
  const clean::RuleEngine engine(machine.tree, machine.environment, machine.usage,
                                 reference_locations());
  const clean::Evaluation evaluation =
      rule.project_marker ? engine.scan_artifacts(pack.clean, machine.workspaces, machine.git, now)
                          : engine.evaluate(pack.clean, now);
  std::vector<fs::path> paths;
  for (const clean::Candidate& candidate : evaluation.candidates) {
    if (candidate.rule_id == rule.id) {
      paths.push_back(candidate.path);
    }
  }
  return contains(paths);
}

Selects compress_selection(const CompressTarget& target, const Machine& machine) {
  return [folders = resolved(target.paths, machine), &target](const fs::path& path) {
    return std::ranges::any_of(folders, [&](const fs::path& folder) {
      if (!clean::within(path, folder)) {
        return false;
      }
      const fs::path inside = path.lexically_relative(folder);
      return std::ranges::none_of(inside, [&target](const fs::path& component) {
        return std::ranges::any_of(target.exclude, [&component](const std::string& pattern) {
          return clean::wildcard_match(component.native(), from_utf8(pattern).native());
        });
      });
    });
  };
}

template <typename Entry>
const Entry* find_case(const std::vector<Entry>& entries, const std::string& id) {
  const auto found =
      std::ranges::find_if(entries, [&id](const Entry& entry) { return entry.id == id; });
  return found == entries.end() ? nullptr : &*found;
}

// What case `id` of `kind` selects on the machine; fails `fields` when the pack has no such case.
Selects selection(const Pack& pack, Kind kind, const std::string& id, const Machine& machine,
                  const Fields& fields) {
  const auto missing = [&]() -> Selects {
    fields.fail("case", "names no " + std::string(kind_names.at(static_cast<std::size_t>(kind))) +
                            " case of the pack");
  };
  switch (kind) {
    case Kind::clean: {
      const rules::CleaningRule* rule = find_case(pack.clean, id);
      return rule != nullptr ? clean_selection(pack, *rule, machine) : missing();
    }
    case Kind::attribution: {
      const map::Area* area = find_case(pack.attribution, id);
      if (area == nullptr) {
        return missing();
      }
      std::vector<std::string> paths;
      std::ranges::transform(area->paths, std::back_inserter(paths),
                             [](const std::wstring& path) { return utf8(fs::path{path}); });
      return [paths, &machine](const fs::path& path) {
        return names(paths, path, machine.environment);
      };
    }
    case Kind::relocate: {
      const RelocationRecipe* recipe = find_case(pack.relocate, id);
      if (recipe == nullptr) {
        return missing();
      }
      return [recipe, &machine](const fs::path& path) {
        return names(recipe->locations, path, machine.environment);
      };
    }
    case Kind::compress: {
      const CompressTarget* target = find_case(pack.compress, id);
      return target != nullptr ? compress_selection(*target, machine) : missing();
    }
  }
  return missing();
}

}  // namespace

std::vector<std::string> run_fixture(const Pack& pack, std::string_view text,
                                     const fs::path& file) {
  std::vector<std::string> failures;
  try {
    const toml::table root = detail::parse_document(text, file);
    const Fields fields(root, utf8(file));
    fields.only({"env", "workspace", "entry", "expect"});
    Machine machine;
    read_machine(fields, machine);
    const std::vector<const toml::table*> expects = fields.tables("expect");
    if (expects.empty()) {
      fields.fail("expect", "must list at least one expectation ([[expect]])");
    }
    int number = 0;
    for (const toml::table* table : expects) {
      Fields expect(*table, fields.where() + ": expect #" + std::to_string(++number));
      expect.only({"kind", "case", "match", "no_match"});
      const Kind kind = expect.required_choice<Kind>("kind", kind_names);
      const std::string id = pack.manifest.id + "/" + expect.required_string("case");
      expect = Fields(*table, expect.where() + " (" +
                                  std::string(kind_names.at(static_cast<std::size_t>(kind))) +
                                  " '" + id + "')");
      const std::vector<std::string> match = expect.strings("match");
      const std::vector<std::string> no_match = expect.strings("no_match");
      if (match.empty() && no_match.empty()) {
        expect.fail("", "sets neither match nor no_match");
      }
      const Selects selects = selection(pack, kind, id, machine, expect);
      for (const std::string& path : match) {
        if (!selects(from_utf8(path))) {
          failures.push_back(expect.where() + ": '" + path + "' is not selected");
        }
      }
      for (const std::string& path : no_match) {
        if (selects(from_utf8(path))) {
          failures.push_back(expect.where() + ": '" + path + "' is selected");
        }
      }
    }
  } catch (const detail::SchemaError& error) {
    failures.emplace_back(error.what());
  }
  return failures;
}

std::vector<std::string> run_fixtures(const Pack& pack, int& count) {
  count = 0;
  const rules::DiskDataFiles files;
  const Result<std::vector<fs::path>> listed = files.list(pack.folder / "tests");
  if (!listed) {
    return listed.error().code == Errc::not_found ? std::vector<std::string>{}
                                                  : std::vector{listed.error().message};
  }
  std::vector<std::string> failures;
  for (const fs::path& file : *listed) {
    if (file.extension() != ".toml") {
      failures.push_back(utf8(file) + ": is not a fixture (tests hold *.toml only)");
      continue;
    }
    ++count;
    const Result<std::string> text = files.read(file);
    std::ranges::move(text ? run_fixture(pack, *text, file) : std::vector{text.error().message},
                      std::back_inserter(failures));
  }
  return failures;
}

}  // namespace myclean::packs::testing
