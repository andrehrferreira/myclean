#include "cli/packs_command.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "cli/cli.hpp"
#include "cli/map_command.hpp"
#include "core/model/result.hpp"
#include "core/model/text.hpp"
#include "core/packs/applies.hpp"
#include "core/packs/fixtures.hpp"
#include "core/packs/loader.hpp"
#include "core/packs/registry.hpp"
#include "core/packs/schema.hpp"
#include "platform/clean/win32_probes.hpp"
#include "platform/packs/win32_packs.hpp"

namespace myclean::cli {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

std::string utf8(const fs::path& path) {
  return utf8_text(path);
}

fs::path from_utf8(const std::string& text) {
  return fs::path{std::u8string(text.begin(), text.end())};
}

struct Request {
  std::string command;           // list | validate
  std::optional<fs::path> path;  // validate: a pack folder or a folder of packs
  std::optional<fs::path> data_dir;
  bool json = false;
};

Result<Request> parse(std::span<const std::string> args) {
  Request request;
  for (std::size_t index = 0; index < args.size(); ++index) {
    const std::string& arg = args[index];
    if (arg == "--json") {
      request.json = true;
    } else if (arg == "--data" && index + 1 < args.size()) {
      request.data_dir = from_utf8(args[++index]);
    } else if (arg.starts_with("--")) {
      return fail(Errc::invalid_argument, "packs: unknown or incomplete option " + arg);
    } else if (request.command.empty()) {
      request.command = arg;
    } else if (request.command == "validate" && !request.path) {
      request.path = from_utf8(arg);
    } else {
      return fail(Errc::invalid_argument, "packs: unexpected argument " + arg);
    }
  }
  if (request.command != "list" && request.command != "validate") {
    return fail(Errc::invalid_argument, "packs: use `packs list` or `packs validate [<path>]`");
  }
  return request;
}

const packs::Pack* find_pack(const packs::LoadReport& report, const packs::PackOutcome& outcome) {
  if (outcome.status != packs::PackStatus::loaded) {
    return nullptr;
  }
  const auto found = std::ranges::find_if(report.packs, [&](const packs::Pack& pack) {
    return pack.manifest.id == outcome.id && pack.origin == outcome.origin;
  });
  return found == report.packs.end() ? nullptr : &*found;
}

Json cases_of(const packs::Pack& pack) {
  return Json{{"clean", pack.clean.size()},
              {"attribution", pack.attribution.size()},
              {"relocate", pack.relocate.size()},
              {"compress", pack.compress.size()},
              {"kb", pack.kb.size()},
              {"classes", pack.classes.size()}};
}

Json errors_json(const std::vector<Error>& errors) {
  Json list = Json::array();
  for (const Error& error : errors) {
    list.push_back(error.message);
  }
  return list;
}

// One row of `packs list`.
Json list_row(const LocalPacks& local, const packs::PackOutcome& outcome) {
  const packs::Pack* pack = find_pack(local.report, outcome);
  if (pack == nullptr) {
    return Json{{"id", outcome.id},
                {"version", nullptr},
                {"origin", packs::to_string(outcome.origin)},
                {"status", packs::to_string(outcome.status)},
                {"applies", false},
                {"cases", nullptr},
                {"message", outcome.message.empty() ? Json() : Json(outcome.message)}};
  }
  return Json{{"id", outcome.id},
              {"version", packs::to_string(pack->manifest.version)},
              {"origin", packs::to_string(outcome.origin)},
              {"status", packs::to_string(outcome.status)},
              {"applies",
               std::ranges::find(local.registry.packs, outcome.id) != local.registry.packs.end()},
              {"cases", cases_of(*pack)},
              {"message", nullptr}};
}

// "1 clean, 2 attribution": the kinds a pack has cases of.
std::string cases_text(const Json& cases) {
  std::string text;
  for (const auto& [kind, count] : cases.items()) {
    if (count.get<std::size_t>() > 0) {
      text += std::format("{}{} {}", text.empty() ? "" : ", ", count.get<std::size_t>(), kind);
    }
  }
  return text;
}

void print_list(const Json& rows, std::ostream& out) {
  out << std::format("{:<20} {:<8} {:<8} {:<9} {:<7} {}\n", "PACK", "VERSION", "ORIGIN", "STATUS",
                     "APPLIES", "CASES");
  for (const Json& row : rows) {
    const bool loaded = row.at("version").is_string();
    std::string detail;  // the case counts of a loaded pack, why another was rejected
    if (loaded) {
      detail = cases_text(row.at("cases"));
    } else if (row.at("message").is_string()) {
      detail = row.at("message").get<std::string>();
    }
    out << std::format("{:<20} {:<8} {:<8} {:<9} {:<7} {}\n", row.at("id").get<std::string>(),
                       loaded ? row.at("version").get<std::string>() : "-",
                       row.at("origin").get<std::string>(), row.at("status").get<std::string>(),
                       row.at("applies").get<bool>() ? "yes" : "no", detail);
  }
}

int list(const Request& request, std::ostream& out, std::ostream& err) {
  const LocalPacks local = load_local_packs(request.data_dir.value_or(default_data_dir()));
  Json rows = Json::array();
  for (const packs::PackOutcome& outcome : local.report.outcomes) {
    rows.push_back(list_row(local, outcome));
  }
  if (request.json) {
    out << Json{{"packs", rows}, {"errors", errors_json(local.report.errors)}}.dump() << '\n';
  } else {
    print_list(rows, out);
    for (const Error& error : local.report.errors) {
      err << "myclean: packs: " << error.message << '\n';
    }
  }
  return local.report.errors.empty() ? exit_ok : exit_unavailable;
}

// One validated pack: its own error, or its fixtures' failures.
Json validated(const std::string& id, const Result<packs::Pack>& pack) {
  Json row{{"id", id}, {"fixtures", 0}, {"failures", Json::array()}};
  if (!pack) {
    row["failures"].push_back(pack.error().message);
  } else {
    const packs::FixtureRun run = packs::run_fixtures(packs::DiskPackFiles{}, *pack);
    row["fixtures"] = run.fixtures;
    row["failures"] = run.failures;
    if (run.fixtures == 0) {
      row["failures"].push_back("pack '" + id + "': has no fixture in tests\\");
    }
  }
  row["valid"] = row.at("failures").empty();
  return row;
}

// Every pack of the folder of packs `path`, loaded together, and what made none load there.
Json validated_folder(const fs::path& path) {
  const packs::LoadReport report =
      packs::load_packs(packs::DiskPackFiles{}, path, std::nullopt, packs::this_version());
  Json rows = Json::array();
  for (const packs::PackOutcome& outcome : report.outcomes) {
    const packs::Pack* pack = find_pack(report, outcome);
    rows.push_back(validated(outcome.id, pack != nullptr
                                             ? Result<packs::Pack>(*pack)
                                             : fail(Errc::invalid_data, outcome.message)));
  }
  for (const Error& problem : report.errors) {
    const bool of_a_pack = std::ranges::any_of(
        report.outcomes, [&](const auto& outcome) { return outcome.message == problem.message; });
    if (!of_a_pack) {
      rows.push_back(Json{{"id", utf8(path)},
                          {"fixtures", 0},
                          {"failures", Json::array({problem.message})},
                          {"valid", false}});
    }
  }
  return rows;
}

void print_validation(const Json& rows, std::ostream& out) {
  for (const Json& row : rows) {
    const int fixtures = row.at("fixtures").get<int>();
    out << std::format("{} {}: {} fixture{}\n", row.at("valid").get<bool>() ? "ok  " : "FAIL",
                       row.at("id").get<std::string>(), fixtures, fixtures == 1 ? "" : "s");
    for (const Json& failure : row.at("failures")) {
      out << "     " << failure.get<std::string>() << '\n';
    }
  }
}

int validate(const Request& request, std::ostream& out) {
  const fs::path path =
      request.path.value_or(request.data_dir.value_or(default_data_dir()) / "packs");
  std::error_code error;
  const Json rows = fs::exists(path / "pack.toml", error)
                        ? Json::array({validated(
                              utf8(path.filename()),
                              packs::load_pack(packs::DiskPackFiles{}, path, packs::Origin::builtin,
                                               packs::this_version()))})
                        : validated_folder(path);
  const bool valid =
      std::ranges::all_of(rows, [](const Json& row) { return row.at("valid").get<bool>(); });
  if (request.json) {
    out << Json{{"valid", valid}, {"packs", rows}}.dump() << '\n';
  } else {
    print_validation(rows, out);
  }
  return valid ? exit_ok : exit_unavailable;
}
}  // namespace

LocalPacks load_local_packs(const fs::path& data_dir) {
  LocalPacks local;
  local.report =
      packs::load_installed(packs::DiskPackFiles{}, data_dir,
                            platform::owner_packs_dir().and_then(platform::owner_packs_folder));
  const platform::Win32Machine machine;
  const platform::Win32FileTree tree;
  const platform::Win32Environment environment;
  local.registry = packs::make_registry(
      local.report.packs,
      packs::Probes{.machine = &machine, .tree = &tree, .environment = &environment});
  return local;
}

int packs_command(std::span<const std::string> args, std::ostream& out, std::ostream& err) {
  const Result<Request> request = parse(args);
  if (!request) {
    err << "myclean: " << request.error().message << '\n';
    return exit_usage;
  }
  return request->command == "list" ? list(*request, out, err) : validate(*request, out);
}

}  // namespace myclean::cli
