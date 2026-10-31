#include "core/packs/loader.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include "core/map/attribution.hpp"
#include "core/model/enum_names.hpp"
#include "core/model/result.hpp"
#include "core/packs/fields.hpp"
#include "core/packs/parse.hpp"
#include "core/packs/schema.hpp"
#include "core/rules/data_files.hpp"
#include "core/rules/parse.hpp"
#include "core/rules/schema.hpp"

namespace myclean::packs {

namespace {

namespace fs = std::filesystem;
using detail::utf8;

constexpr std::array<std::string_view, 4> status_names = {"loaded", "disabled", "shadowed",
                                                          "rejected"};
constexpr std::array<std::string_view, 7> pack_files = {
    "pack.toml",     "clean.toml", "attribution.toml", "relocate.toml",
    "compress.toml", "kb.json",    "classes.toml"};

std::string folded(std::string_view text) {
  std::string result(text);
  std::ranges::transform(result, result.begin(), [](char character) {
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                : character;
  });
  return result;
}

bool valid_case_id(std::string_view id) {
  return !id.empty() && std::ranges::all_of(id, [](char character) {
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
           character == '.' || character == '_' || character == '-';
  });
}

std::string join(const std::vector<std::string>& values) {
  std::string text;
  for (const std::string& value : values) {
    text += (text.empty() ? "" : ",") + value;
  }
  return text;
}

// What one case acts on: two cases with the same `key` conflict.
struct Claim {
  std::string key;  // folded `what`
  std::string what;
  std::string owner;  // "<pack>/<case>"
};

// Records that `owner` claims "<kind> '<value>'".
void claim(std::vector<Claim>& claims, const std::string& owner, std::string_view kind,
           std::string_view value) {
  std::string what(kind);
  what.append(" '").append(value).append("'");
  claims.push_back(Claim{.key = folded(what), .what = std::move(what), .owner = owner});
}

std::vector<Claim> claims_of(const Pack& pack) {
  std::vector<Claim> claims;
  for (const rules::CleaningRule& rule : pack.clean) {
    std::string kind = "clean target";
    if (rule.project_marker) {
      kind.append(" of ").append(*rule.project_marker).append(" projects");
    }
    for (const std::string& target : rule.targets) {
      claim(claims, rule.id, kind, target);
    }
    if (!rule.command.empty()) {
      claim(claims, rule.id, "native command", rule.command);
    }
    if (!rule.handler.empty()) {
      claim(claims, rule.id, "cleanup handler", rule.handler);
    }
  }
  for (const map::Area& area : pack.attribution) {
    for (const std::wstring& path : area.paths) {
      claim(claims, area.id, "attribution path", utf8(fs::path{path}));
    }
  }
  for (const RelocationRecipe& recipe : pack.relocate) {
    std::string kind = "relocation setting ";
    kind.append(to_string(recipe.current.kind));
    std::string value = recipe.current.text;
    if (!recipe.current.key.empty()) {
      value.append("' key '").append(recipe.current.key);
    }
    claim(claims, recipe.id, kind, value);
  }
  for (const CompressTarget& target : pack.compress) {
    for (const std::string& path : target.paths) {
      claim(claims, target.id, "compression path", path);
    }
  }
  for (const rules::KbEntry& entry : pack.kb) {
    std::string value = entry.image;
    value.append("' path '")
        .append(entry.path ? entry.path->text : "")
        .append("' signer '")
        .append(entry.signer.value_or(""));
    claim(claims, entry.id, "knowledge-base image", value);
  }
  for (const rules::ClassMatch& match : pack.classes) {
    std::string value = join(match.images);
    value.append("' command_line '")
        .append(match.command_line ? match.command_line->text : "")
        .append("' path_root '")
        .append(join(match.path_roots))
        .append("' signer '")
        .append(join(match.signers));
    claim(claims, match.id, "class criteria image", value);
  }
  return claims;
}

// Checks every case id of a content file and namespaces it; fails `file` otherwise.
template <typename Entry>
Result<void> namespace_ids(std::vector<Entry>& entries, const std::string& pack,
                           const fs::path& file) {
  for (Entry& entry : entries) {
    if (!valid_case_id(entry.id)) {
      return fail(Errc::invalid_data, utf8(file) + ": case '" + entry.id +
                                          "': field 'id' must hold only a-z, 0-9, '.', '_' "
                                          "and '-'");
    }
    entry.id = pack + "/" + entry.id;
  }
  return {};
}

bool has_case(const auto& entries, std::string_view id) {
  return std::ranges::any_of(entries, [id](const auto& entry) { return entry.id == id; });
}

// "<file>: area '<id>': field 'reclaim.ref' <problem>: '<ref>'"
Error ref_error(const fs::path& file, const map::Area& area, std::string_view problem) {
  std::string message = utf8(file);
  message.append(": area '")
      .append(area.id)
      .append("': field 'reclaim.ref' ")
      .append(problem)
      .append(": '")
      .append(area.reclaim.ref)
      .append("'");
  return Error{.code = Errc::invalid_data, .message = std::move(message)};
}

// Namespaces the reclaim refs of the pack's attribution areas (see load_pack).
Result<void> resolve_refs(Pack& pack, const fs::path& file) {
  const std::string prefix = pack.manifest.id + "/";
  for (map::Area& area : pack.attribution) {
    std::string& ref = area.reclaim.ref;
    const bool foreign = ref.find('/') != std::string::npos;
    if (area.reclaim.kind == map::ReclaimKind::clean_rule) {
      if (foreign || !has_case(pack.clean, prefix + ref)) {
        return tl::unexpected(ref_error(file, area, "names no clean case of this pack"));
      }
      ref.insert(0, prefix);
    } else if (area.reclaim.kind == map::ReclaimKind::relocate) {
      if (foreign) {
        return tl::unexpected(ref_error(file, area, "names a case of another pack"));
      }
      if (has_case(pack.relocate, prefix + ref)) {
        ref.insert(0, prefix);
      }
    }
  }
  return {};
}

Result<void> check_no_caps(std::string_view text, const fs::path& file) {
  try {
    if (detail::parse_document(text, file).contains("caps")) {
      return fail(Errc::invalid_data,
                  utf8(file) + ": field 'caps' is not allowed in a pack: job caps are global");
    }
    return {};
  } catch (const detail::SchemaError& error) {
    return fail(Errc::invalid_data, error.what());
  }
}

// Reads one content file into `pack`.
Result<void> load_content(Pack& pack, std::string_view name, const std::string& text,
                          const fs::path& file) {
  const std::string& id = pack.manifest.id;
  const auto into = [&](auto& entries, auto parsed) -> Result<void> {
    if (!parsed) {
      return tl::unexpected(parsed.error());
    }
    entries = std::move(*parsed);
    return namespace_ids(entries, id, file);
  };
  if (name == "clean.toml") {
    return into(pack.clean, rules::parse_rule_file(text, file));
  }
  if (name == "attribution.toml") {
    return into(pack.attribution, map::parse_attribution_file(text, file));
  }
  if (name == "relocate.toml") {
    return into(pack.relocate, parse_relocate_file(text, file));
  }
  if (name == "compress.toml") {
    return into(pack.compress, parse_compress_file(text, file));
  }
  if (name == "kb.json") {
    return into(pack.kb, rules::parse_kb_file(text, file));
  }
  return check_no_caps(text, file).and_then([&] {
    return rules::parse_class_file(text, file).and_then([&](rules::ClassSet classes) {
      return into(pack.classes, Result<std::vector<rules::ClassMatch>>(std::move(classes.matches)));
    });
  });
}

// The pack itself, before the pack-wide checks.
Result<Pack> read_pack(const rules::IDataFiles& files, const fs::path& folder, Origin origin,
                       const Version& running) {
  const Result<std::vector<fs::path>> listed = files.list(folder);
  if (!listed) {
    return tl::unexpected(listed.error());
  }
  std::vector<fs::path> content = *listed;
  std::ranges::sort(content);
  for (const fs::path& file : content) {
    if (std::ranges::find(pack_files, utf8(file.filename())) == pack_files.end()) {
      return fail(Errc::invalid_data, utf8(file) +
                                          ": is not a pack file (pack.toml, clean.toml, "
                                          "attribution.toml, relocate.toml, "
                                          "compress.toml, kb.json, classes.toml)");
    }
  }
  const fs::path manifest_file = folder / "pack.toml";
  if (std::ranges::find(content, manifest_file) == content.end()) {
    return fail(Errc::invalid_data, "has no pack.toml");
  }
  Pack pack;
  pack.origin = origin;
  pack.folder = folder;
  Result<Manifest> manifest =
      files.read(manifest_file).and_then([&manifest_file](const std::string& text) {
        return parse_manifest(text, manifest_file);
      });
  if (!manifest) {
    return tl::unexpected(manifest.error());
  }
  pack.manifest = std::move(*manifest);
  if (pack.manifest.id != utf8(folder.filename())) {
    return fail(Errc::invalid_data, utf8(manifest_file) +
                                        ": field 'id' must be the folder's name, "
                                        "not '" +
                                        pack.manifest.id + "'");
  }
  if (running < pack.manifest.min_myclean) {
    return fail(Errc::refused, "needs MyClean " + to_string(pack.manifest.min_myclean) +
                                   " or newer; this is " + to_string(running));
  }
  if (content.size() == 1) {
    return fail(Errc::invalid_data, "has no content file");
  }
  for (const fs::path& file : content) {
    const std::string name = utf8(file.filename());
    if (name == "pack.toml") {
      continue;
    }
    const Result<void> loaded = files.read(file).and_then(
        [&](const std::string& text) { return load_content(pack, name, text, file); });
    if (!loaded) {
      return tl::unexpected(loaded.error());
    }
  }
  if (Result<void> refs = resolve_refs(pack, folder / "attribution.toml"); !refs) {
    return tl::unexpected(refs.error());
  }
  std::map<std::string, const Claim*> seen;
  const std::vector<Claim> claims = claims_of(pack);
  const auto twice = std::ranges::find_if(claims, [&seen](const Claim& claim) {
    const auto [found, inserted] = seen.emplace(claim.key, &claim);
    return !inserted && found->second->owner != claim.owner;
  });
  if (twice != claims.end()) {
    return fail(Errc::invalid_data, "case '" + twice->owner + "' claims " + twice->what +
                                        ", as case '" + seen.at(twice->key)->owner + "' does");
  }
  return pack;
}

Result<std::set<std::string, std::less<>>> read_settings(const IPackFiles& files,
                                                         const fs::path& owner_dir) {
  const fs::path file = owner_dir / settings_file;
  const Result<std::string> text = files.read(file);
  if (!text && text.error().code == Errc::not_found) {
    return std::set<std::string, std::less<>>{};
  }
  if (!text) {
    return tl::unexpected(text.error());
  }
  try {
    const toml::table root = detail::parse_document(*text, file);
    const detail::Fields fields(root, utf8(file));
    fields.only({"disabled"});
    const std::vector<std::string> disabled = fields.strings("disabled");
    return std::set<std::string, std::less<>>(disabled.begin(), disabled.end());
  } catch (const detail::SchemaError& error) {
    return fail(Errc::invalid_data, error.what());
  }
}

// One load_packs() run.
struct LoadState {
  const IPackFiles* files = nullptr;
  Version running;
  std::set<std::string, std::less<>> disabled;
  std::map<std::string, Claim> claimed;  // by key, of every loaded pack
  std::set<std::string, std::less<>> loaded;
  LoadReport report;
};

// Loads the pack of `outcome` unless it is disabled or shadowed, and sets its status; the error
// that rejects it, if any.
std::optional<Error> admit(LoadState& state, PackOutcome& outcome) {
  if (state.disabled.contains(outcome.id)) {
    outcome.status = PackStatus::disabled;
    return std::nullopt;
  }
  if (state.loaded.contains(outcome.id)) {
    outcome.status = PackStatus::shadowed;
    return std::nullopt;
  }
  Result<Pack> pack = load_pack(*state.files, outcome.folder, outcome.origin, state.running);
  if (!pack) {
    return pack.error();
  }
  const std::vector<Claim> claims = claims_of(*pack);
  const auto conflict = std::ranges::find_if(
      claims, [&state](const Claim& claim) { return state.claimed.contains(claim.key); });
  if (conflict != claims.end()) {
    return Error{.code = Errc::invalid_data,
                 .message = "pack '" + outcome.id + "': case '" + conflict->owner + "' claims " +
                            conflict->what + ", already claimed by case '" +
                            state.claimed.at(conflict->key).owner + "'"};
  }
  for (const Claim& claim : claims) {
    state.claimed.emplace(claim.key, claim);
  }
  state.loaded.insert(outcome.id);
  state.report.packs.push_back(std::move(*pack));
  return std::nullopt;
}

// Loads every pack folder of `dir`, by name.
void visit(LoadState& state, const fs::path& dir, Origin origin) {
  Result<std::vector<fs::path>> folders = state.files->folders(dir);
  if (!folders) {
    if (origin == Origin::builtin || folders.error().code != Errc::not_found) {
      state.report.errors.push_back(folders.error());
    }
    return;
  }
  std::ranges::sort(*folders);
  for (const fs::path& folder : *folders) {
    PackOutcome outcome;
    outcome.id = utf8(folder.filename());
    outcome.folder = folder;
    outcome.origin = origin;
    if (std::optional<Error> error = admit(state, outcome)) {
      outcome.status = PackStatus::rejected;
      outcome.message = error->message;
      state.report.errors.push_back(std::move(*error));
    }
    state.report.outcomes.push_back(std::move(outcome));
  }
}

}  // namespace

Result<std::vector<fs::path>> DiskPackFiles::list(const fs::path& dir) const {
  return files_.list(dir);
}

Result<std::string> DiskPackFiles::read(const fs::path& file) const {
  return files_.read(file);
}

Result<std::vector<fs::path>> DiskPackFiles::folders(const fs::path& dir) const {
  std::error_code error;
  fs::directory_iterator entry(dir, error);
  std::vector<fs::path> result;
  while (!error && entry != fs::directory_iterator()) {
    std::error_code ignored;  // an entry that vanished meanwhile is simply not listed
    if (entry->is_directory(ignored)) {
      result.push_back(entry->path());
    }
    entry.increment(error);
  }
  if (error) {
    return fail(error == std::errc::no_such_file_or_directory ? Errc::not_found : Errc::io_error,
                utf8(dir) + ": cannot be listed: " + error.message());
  }
  return result;
}

std::string_view to_string(PackStatus status) {
  return myclean::detail::enum_name(status_names, status);
}

Result<Pack> load_pack(const rules::IDataFiles& files, const fs::path& folder, Origin origin,
                       const Version& running) {
  Result<Pack> pack = read_pack(files, folder, origin, running);
  if (!pack) {
    return fail(pack.error().code,
                "pack '" + utf8(folder.filename()) + "': " + pack.error().message);
  }
  return pack;
}

LoadReport load_packs(const IPackFiles& files, const fs::path& builtin_dir,
                      const std::optional<fs::path>& owner_dir, const Version& running) {
  LoadState state;
  state.files = &files;
  state.running = running;
  if (owner_dir) {
    Result<std::set<std::string, std::less<>>> settings = read_settings(files, *owner_dir);
    if (settings) {
      state.disabled = std::move(*settings);
    } else {
      state.report.errors.push_back(settings.error());
    }
    visit(state, *owner_dir, Origin::owner);
  }
  visit(state, builtin_dir, Origin::builtin);
  return std::move(state.report);
}

}  // namespace myclean::packs
