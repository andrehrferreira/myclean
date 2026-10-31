#include "core/placement/relocation.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/enum_names.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/npmrc.hpp"
#include "core/placement/settings.hpp"
#include "core/safety/paths.hpp"

namespace myclean::placement {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::array<std::string_view, 9> cache_kind_names = {
    "temp", "gradle", "cargo", "npm", "nuget", "pip", "go_modules", "go_build", "unreal_ddc"};
constexpr std::array<std::string_view, 2> content_mode_names = {"move", "regenerate"};

struct KindInfo {
  std::string_view label;
  std::string_view variable;      // the environment variable that relocates it
  std::string_view default_path;  // where the tool puts it when the variable is not set
  std::string_view folder;        // below the target volume's root
};

constexpr std::array<KindInfo, 9> kind_infos = {{
    {.label = "TEMP folder",
     .variable = "TEMP",
     .default_path = R"(%USERPROFILE%\AppData\Local\Temp)",
     .folder = "Temp"},
    {.label = "Gradle user home",
     .variable = "GRADLE_USER_HOME",
     .default_path = R"(%USERPROFILE%\.gradle)",
     .folder = R"(DevCaches\gradle)"},
    {.label = "Cargo home",
     .variable = "CARGO_HOME",
     .default_path = R"(%USERPROFILE%\.cargo)",
     .folder = R"(DevCaches\cargo)"},
    {.label = "npm cache",
     .variable = "npm_config_cache",
     .default_path = R"(%LOCALAPPDATA%\npm-cache)",
     .folder = R"(DevCaches\npm-cache)"},
    {.label = "NuGet package folder",
     .variable = "NUGET_PACKAGES",
     .default_path = R"(%USERPROFILE%\.nuget\packages)",
     .folder = R"(DevCaches\nuget-packages)"},
    {.label = "pip cache",
     .variable = "PIP_CACHE_DIR",
     .default_path = R"(%LOCALAPPDATA%\pip\Cache)",
     .folder = R"(DevCaches\pip-cache)"},
    {.label = "Go module cache",
     .variable = "GOMODCACHE",
     .default_path = R"(%USERPROFILE%\go\pkg\mod)",
     .folder = R"(DevCaches\go-mod)"},
    {.label = "Go build cache",
     .variable = "GOCACHE",
     .default_path = R"(%LOCALAPPDATA%\go-build)",
     .folder = R"(DevCaches\go-build)"},
    {.label = "Unreal local DDC",
     .variable = "UE-LocalDataCachePath",
     .default_path = R"(%LOCALAPPDATA%\UnrealEngine\Common\DerivedDataCache)",
     .folder = R"(DevCaches\unreal-ddc)"},
}};

constexpr std::string_view npmrc_key = "cache";
constexpr std::string_view path_variable = "Path";

const KindInfo& info(CacheKind kind) {
  return kind_infos.at(static_cast<std::size_t>(kind));
}

// Where a setting is stored.
enum class Store { environment, npmrc };

// One setting a relocation changes, with its value before and after (nullopt: not set).
struct Change {
  Store store = Store::environment;
  std::string name;  // the variable, or the .npmrc key
  fs::path file;     // the .npmrc file
  bool file_existed = false;
  std::optional<EnvValue> before;
  std::optional<EnvValue> after;
};

// The undo data of one applied relocation.
struct Record {
  CacheKind kind = CacheKind::temp;
  fs::path source;
  fs::path target;
  ContentMode content = ContentMode::regenerate;
  std::vector<Change> changes;
};

std::string utf8(const fs::path& path) {
  return safety::to_utf8(path);
}

fs::path from_utf8(std::string_view text) {
  return fs::path{std::u8string(text.begin(), text.end())};
}

Result<fs::path> resolve(const IUserEnvironment& environment, std::string_view text) {
  Result<fs::path> expanded = environment.expand(text);
  if (!expanded) {
    return tl::unexpected(expanded.error());
  }
  return safety::normalize_path(*expanded);
}

// The settings that place `kind`, with their current values in `before`; the one that decides
// the location comes first.
Result<std::vector<Change>> settings_of(const Tools& tools, CacheKind kind) {
  std::vector<Change> changes;
  const auto add_variable = [&](std::string_view name) -> Result<void> {
    Result<std::optional<EnvValue>> value = tools.environment->get(name);
    if (!value) {
      return tl::unexpected(value.error());
    }
    changes.push_back(Change{.name = std::string(name), .before = *std::move(value)});
    return {};
  };
  if (Result<void> added = add_variable(info(kind).variable); !added) {
    return tl::unexpected(added.error());
  }
  if (kind == CacheKind::temp) {
    if (Result<void> added = add_variable("TMP"); !added) {
      return tl::unexpected(added.error());
    }
  }
  if (kind != CacheKind::npm || changes.front().before) {
    return changes;
  }
  // npm_config_cache is not set: the user .npmrc decides, as `npm config set cache` writes it.
  Result<fs::path> file = tools.environment->expand(R"(%USERPROFILE%\.npmrc)");
  if (!file) {
    return tl::unexpected(file.error());
  }
  Result<std::optional<std::string>> text = tools.files->read(*file);
  if (!text) {
    return tl::unexpected(text.error());
  }
  std::optional<EnvValue> before;
  if (*text) {
    if (std::optional<std::string> value = npmrc_value(**text, npmrc_key)) {
      before = EnvValue{.text = *std::move(value)};
    }
  }
  changes.front() = Change{.store = Store::npmrc,
                           .name = std::string(npmrc_key),
                           .file = *std::move(file),
                           .file_existed = text->has_value(),
                           .before = std::move(before)};
  return changes;
}

std::string setting_label(CacheKind kind, const Change& primary) {
  if (primary.store == Store::npmrc) {
    return "npm config cache";
  }
  switch (kind) {
    case CacheKind::temp:
      return "TEMP, TMP";
    case CacheKind::cargo:
      return "CARGO_HOME, PATH";
    default:
      return primary.name;
  }
}

// The location used when the setting is not set. Go puts its module cache below GOPATH.
Result<std::string> default_location(const Tools& tools, CacheKind kind) {
  if (kind != CacheKind::go_modules) {
    return std::string(info(kind).default_path);
  }
  Result<std::optional<EnvValue>> gopath = tools.environment->get("GOPATH");
  if (!gopath) {
    return tl::unexpected(gopath.error());
  }
  const std::optional<EnvValue>& value = *gopath;
  if (!value || value->text.empty()) {
    return std::string(info(kind).default_path);
  }
  return value->text.substr(0, value->text.find(';')) + R"(\pkg\mod)";
}

Result<std::optional<EnvValue>> read_setting(const Tools& tools, const Change& change) {
  if (change.store == Store::environment) {
    return tools.environment->get(change.name);
  }
  Result<std::optional<std::string>> text = tools.files->read(change.file);
  if (!text) {
    return tl::unexpected(text.error());
  }
  std::optional<EnvValue> value;
  if (*text) {
    if (std::optional<std::string> found = npmrc_value(**text, change.name)) {
      value = EnvValue{.text = *std::move(found)};
    }
  }
  return value;
}

Result<void> write_setting(const Tools& tools, const Change& change,
                           const std::optional<EnvValue>& value) {
  if (change.store == Store::environment) {
    return value ? tools.environment->set(change.name, *value)
                 : tools.environment->remove(change.name);
  }
  Result<std::optional<std::string>> text = tools.files->read(change.file);
  if (!text) {
    return tl::unexpected(text.error());
  }
  const std::string current = text->value_or("");
  const std::string updated =
      value ? npmrc_set(current, change.name, value->text) : npmrc_remove(current, change.name);
  if (!value && !change.file_existed && updated.find_first_not_of(" \t\r\n") == std::string::npos) {
    return tools.files->remove(change.file);  // the file MyClean created goes away again
  }
  return tools.files->write(change.file, updated);
}

// `path` (a PATH value) with every entry that resolves to `from` replaced by `to`; nullopt when
// no entry does.
std::optional<std::string> replace_path_entry(const IUserEnvironment& environment,
                                              std::string_view path, const fs::path& from,
                                              const fs::path& to) {
  std::string result;
  bool replaced = false;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = std::min(path.find(';', start), path.size());
    const std::string_view entry = path.substr(start, end - start);
    if (start > 0) {
      result += ';';
    }
    if (const Result<fs::path> resolved = resolve(environment, entry);
        !entry.empty() && resolved && same_path(*resolved, from)) {
      result += utf8(to);
      replaced = true;
    } else {
      result += entry;
    }
    start = end + 1;
  }
  return replaced ? std::optional<std::string>{std::move(result)} : std::nullopt;
}

Result<std::vector<Change>> planned_changes(const Tools& tools, const CacheLocation& location,
                                            const fs::path& target) {
  Result<std::vector<Change>> changes = settings_of(tools, location.kind);
  if (!changes) {
    return changes;
  }
  for (Change& change : *changes) {
    change.after =
        EnvValue{.text = utf8(target), .expandable = change.before && change.before->expandable};
  }
  if (location.kind != CacheKind::cargo) {
    return changes;
  }
  Result<std::optional<EnvValue>> path = tools.environment->get(path_variable);
  if (!path) {
    return tl::unexpected(path.error());
  }
  const std::optional<EnvValue>& value = *path;
  if (!value) {
    return changes;
  }
  if (std::optional<std::string> fixed = replace_path_entry(
          *tools.environment, value->text, location.path / "bin", target / "bin")) {
    changes->push_back(
        Change{.name = std::string(path_variable),
               .before = value,
               .after = EnvValue{.text = *std::move(fixed), .expandable = value->expandable}});
  }
  return changes;
}

Result<TreeTotals> totals(const os::IFileSystem& file_system, const fs::path& root) {
  TreeTotals result;
  std::vector<fs::path> folders{root};
  while (!folders.empty()) {
    const fs::path folder = std::move(folders.back());
    folders.pop_back();
    Result<std::vector<os::DirEntry>> entries = file_system.enumerate(folder);
    if (!entries) {
      return tl::unexpected(entries.error());
    }
    for (os::DirEntry& entry : *entries) {
      if (entry.stat.attributes.reparse_point) {
        continue;
      }
      if (entry.stat.kind == os::EntryKind::directory) {
        folders.push_back(std::move(entry.path));
      } else {
        ++result.files;
        result.bytes += entry.stat.size;
      }
    }
  }
  return result;
}

std::string describe_totals(const TreeTotals& totals) {
  return std::to_string(totals.files) + " files, " + std::to_string(totals.bytes.count()) +
         " bytes";
}

}  // namespace

Result<Bytes> readable_size(const os::IFileSystem& file_system, const fs::path& root) {
  const Result<os::FileStat> status = file_system.stat(root);
  if (!status) {
    return status.error().code == Errc::not_found ? Result<Bytes>{Bytes{}}
                                                  : tl::unexpected(status.error());
  }
  Bytes total;
  std::vector<fs::path> folders;
  if (status->kind == os::EntryKind::directory && !status->attributes.reparse_point) {
    folders.push_back(root);
  }
  while (!folders.empty()) {
    const fs::path folder = std::move(folders.back());
    folders.pop_back();
    Result<std::vector<os::DirEntry>> entries = file_system.enumerate(folder);
    if (!entries) {
      if (folder == root) {
        return tl::unexpected(entries.error());
      }
      continue;
    }
    for (os::DirEntry& entry : *entries) {
      total += entry.stat.size_on_disk;
      if (entry.stat.kind == os::EntryKind::directory && !entry.stat.attributes.reparse_point) {
        folders.push_back(std::move(entry.path));
      }
    }
  }
  return total;
}

Result<void> check_target(const IDriveFacts& drives, const fs::path& target, Bytes needed) {
  Result<std::vector<TargetVolume>> volumes = drives.volumes();
  if (!volumes) {
    return tl::unexpected(volumes.error());
  }
  const TargetVolume* volume = volume_of(*volumes, target);
  if (volume == nullptr) {
    return fail(Errc::refused, "no mounted volume holds " + utf8(target));
  }
  if (const std::string why = unsuitable(*volume, needed); !why.empty()) {
    return fail(Errc::refused, utf8(volume->root) + " is not a safe target: " + why);
  }
  return {};
}

Result<void> copy_content(const Tools& tools, const fs::path& source, const fs::path& target,
                          std::string_view label) {
  const os::IFileSystem& file_system = *tools.file_system;
  if (file_system.stat(target)) {
    Result<std::vector<os::DirEntry>> entries = file_system.enumerate(target);
    if (!entries || !entries->empty()) {
      return fail(Errc::not_empty, "the target " + utf8(target) + " is not an empty folder");
    }
  }
  Result<TreeTotals> expected = totals(file_system, source);
  if (!expected) {
    return tl::unexpected(expected.error());
  }
  Result<TreeTotals> copied = tools.copier->copy_tree(source, target);
  if (!copied) {
    return fail(copied.error().code,
                "copying the " + std::string(label) + " to " + utf8(target) +
                    " failed, so no setting changed: " + copied.error().message);
  }
  Result<TreeTotals> present = totals(file_system, target);
  if (!present || *present != *expected || *copied != *expected) {
    return fail(Errc::io_error, "the copy at " + utf8(target) + " does not match the " +
                                    std::string(label) + " (" + describe_totals(*expected) +
                                    " expected), so no setting changed");
  }
  return {};
}

namespace {

// Tells running programs about changed environment variables. Best effort, see the header.
void announce(const Tools& tools, const std::vector<Change>& changes) {
  if (std::ranges::any_of(
          changes, [](const Change& change) { return change.store == Store::environment; })) {
    static_cast<void>(tools.environment->announce());
  }
}

Json value_json(const std::optional<EnvValue>& value) {
  if (!value) {
    return nullptr;
  }
  return Json{{"text", value->text}, {"expandable", value->expandable}};
}

std::optional<EnvValue> value_from(const Json& json) {
  if (json.is_null()) {
    return std::nullopt;
  }
  return EnvValue{.text = json.at("text").get<std::string>(),
                  .expandable = json.at("expandable").get<bool>()};
}

std::string record_json(const Record& record) {
  Json changes = Json::array();
  for (const Change& change : record.changes) {
    changes.push_back(Json{{"store", change.store == Store::npmrc ? "npmrc" : "environment"},
                           {"name", change.name},
                           {"file", utf8(change.file)},
                           {"file_existed", change.file_existed},
                           {"before", value_json(change.before)},
                           {"after", value_json(change.after)}});
  }
  return Json{{"kind", to_string(record.kind)},
              {"source", utf8(record.source)},
              {"target", utf8(record.target)},
              {"content", to_string(record.content)},
              {"changes", std::move(changes)}}
      .dump();
}

Result<Record> parse_record(const Undo& undo) {
  if (undo.items.size() != 1) {
    return fail(Errc::invalid_data, "relocation undo data must hold exactly one item");
  }
  try {
    const Json json = Json::parse(undo.items.front().data);
    const std::optional<CacheKind> kind = parse_cache_kind(json.at("kind").get<std::string>());
    const std::optional<ContentMode> content =
        parse_content_mode(json.at("content").get<std::string>());
    if (!kind || !content) {
      return fail(Errc::invalid_data, "relocation undo data names an unknown kind or mode");
    }
    Record record{.kind = *kind,
                  .source = from_utf8(json.at("source").get<std::string>()),
                  .target = from_utf8(json.at("target").get<std::string>()),
                  .content = *content};
    for (const Json& change : json.at("changes")) {
      record.changes.push_back(Change{.store = change.at("store").get<std::string>() == "npmrc"
                                                   ? Store::npmrc
                                                   : Store::environment,
                                      .name = change.at("name").get<std::string>(),
                                      .file = from_utf8(change.at("file").get<std::string>()),
                                      .file_existed = change.at("file_existed").get<bool>(),
                                      .before = value_from(change.at("before")),
                                      .after = value_from(change.at("after"))});
    }
    return record;
  } catch (const Json::exception& error) {
    return fail(Errc::invalid_data,
                std::string("relocation undo data is malformed: ") + error.what());
  }
}

std::string display(const std::optional<EnvValue>& value) {
  return value ? "\"" + value->text + "\"" : "not set";
}

// Writes `values(change)` for `changes` in order. When one write fails, the ones already
// written get `restore(change)` back, and the failure is returned.
template <typename Values, typename Restore>
Result<void> write_all(const Tools& tools, const std::vector<Change>& changes, Values values,
                       Restore restore) {
  for (std::size_t index = 0; index < changes.size(); ++index) {
    if (Result<void> written = write_setting(tools, changes[index], values(changes[index]));
        !written) {
      for (std::size_t done = index; done-- > 0;) {
        static_cast<void>(write_setting(tools, changes[done], restore(changes[done])));
      }
      return fail(written.error().code,
                  "writing " + changes[index].name + " failed: " + written.error().message);
    }
  }
  return {};
}

}  // namespace

std::string_view to_string(CacheKind kind) {
  return detail::enum_name(cache_kind_names, kind);
}

std::optional<CacheKind> parse_cache_kind(std::string_view text) {
  return detail::enum_from_name<CacheKind>(cache_kind_names, text);
}

std::string_view to_string(ContentMode mode) {
  return detail::enum_name(content_mode_names, mode);
}

std::optional<ContentMode> parse_content_mode(std::string_view text) {
  return detail::enum_from_name<ContentMode>(content_mode_names, text);
}

Result<CacheLocation> locate(const Tools& tools, CacheKind kind) {
  Result<std::vector<Change>> settings = settings_of(tools, kind);
  if (!settings) {
    return tl::unexpected(settings.error());
  }
  const Change& primary = settings->front();
  Result<std::string> text =
      primary.before ? Result<std::string>{primary.before->text} : default_location(tools, kind);
  if (!text) {
    return tl::unexpected(text.error());
  }
  Result<fs::path> path = resolve(*tools.environment, *text);
  if (!path) {
    return fail(path.error().code, std::string(info(kind).label) + " location \"" + *text +
                                       "\" is not usable: " + path.error().message);
  }
  Result<Bytes> size = readable_size(*tools.file_system, *path);
  if (!size) {
    return tl::unexpected(size.error());
  }
  return CacheLocation{.kind = kind,
                       .setting = setting_label(kind, primary),
                       .path = *std::move(path),
                       .configured = primary.before.has_value(),
                       .size = *size};
}

ContentMode recommended_content(CacheKind kind) {
  return kind == CacheKind::cargo ? ContentMode::move : ContentMode::regenerate;
}

fs::path target_folder(CacheKind kind, const fs::path& root) {
  return root / from_utf8(info(kind).folder);
}

Result<std::vector<RelocationProposal>> propose_relocations(const Tools& tools) {
  Result<std::vector<TargetVolume>> volumes = tools.drives->volumes();
  if (!volumes) {
    return tl::unexpected(volumes.error());
  }
  Result<fs::path> system = tools.environment->expand("%SystemDrive%");
  if (!system) {
    return tl::unexpected(system.error());
  }
  std::vector<RelocationProposal> proposals;
  for (const CacheKind kind : all_cache_kinds) {
    Result<CacheLocation> location = locate(tools, kind);
    if (!location) {
      return tl::unexpected(location.error());
    }
    RelocationProposal proposal{.current = *std::move(location),
                                .content = recommended_content(kind)};
    if (drive_root(proposal.current.path) != drive_root(*system)) {
      proposal.reason = "already off the system drive";
    } else if (const TargetVolume* best =
                   best_target(*volumes, proposal.current.path, proposal.current.size)) {
      proposal.target = target_folder(kind, best->root);
    } else {
      proposal.reason = "no other volume is healthy and has room";
    }
    proposals.push_back(std::move(proposal));
  }
  return proposals;
}

ActionInfo RelocateAction::describe() const {
  const KindInfo& kind = info(relocation_.kind);
  return {.what = "Relocate the " + std::string(kind.label) + " to " + utf8(relocation_.target),
          .why = relocation_.content == ContentMode::move
                     ? "it keeps growing on the system drive; its content is copied and checked "
                       "before the setting switches, and the old folder is then offered for "
                       "cleaning"
                     : "it keeps growing on the system drive; the tool regenerates its content "
                       "at the new location, and the old folder is offered for cleaning",
          .risk = Risk::medium,
          .tier = Tier::ask,
          .context = ExecutionContext::user};
}

Result<Preview> RelocateAction::preview() {
  Result<CacheLocation> location = locate(*tools_, relocation_.kind);
  if (!location) {
    return tl::unexpected(location.error());
  }
  PlanItem item{.path = location->path, .size = location->size};
  if (const Result<os::FileStat> stat = tools_->file_system->stat(location->path); stat) {
    item.identity = stat->identity;
  } else if (stat.error().code != Errc::not_found) {
    return tl::unexpected(stat.error());
  }
  return Preview{.items = {std::move(item)}};
}

Result<Undo> RelocateAction::apply(const Preview& preview) {
  if (preview.items.size() != 1) {
    return fail(Errc::invalid_argument, "a relocation preview holds exactly one item");
  }
  const PlanItem& item = preview.items.front();
  const std::string label{info(relocation_.kind).label};
  Result<CacheLocation> location = locate(*tools_, relocation_.kind);
  if (!location) {
    return tl::unexpected(location.error());
  }
  if (!same_path(location->path, item.path)) {
    return fail(Errc::refused,
                "the " + label + " moved since the preview: it is now at " + utf8(location->path));
  }
  Result<fs::path> target = safety::normalize_path(relocation_.target);
  if (!target) {
    return tl::unexpected(target.error());
  }
  if (same_path(*target, location->path) || inside(*target, location->path) ||
      inside(location->path, *target)) {
    return fail(Errc::invalid_argument,
                "the target " + utf8(*target) + " overlaps the current folder");
  }
  if (relocation_.kind == CacheKind::cargo && relocation_.content == ContentMode::regenerate) {
    return fail(Errc::refused,
                "Cargo keeps its installed tools in CARGO_HOME\\bin, so its content must move");
  }
  // The target's drive is checked now, not when it was proposed: it may have degraded since.
  if (Result<void> safe = check_target(*tools_->drives, *target, location->size); !safe) {
    return tl::unexpected(safe.error());
  }
  Result<std::vector<Change>> changes = planned_changes(*tools_, *location, *target);
  if (!changes) {
    return tl::unexpected(changes.error());
  }
  os::IFileSystem& file_system = *tools_->file_system;
  const Result<os::FileStat> source = file_system.stat(location->path);
  if (!source && source.error().code != Errc::not_found) {
    return tl::unexpected(source.error());
  }
  const Result<void> placed = relocation_.content == ContentMode::move && source
                                  ? copy_content(*tools_, location->path, *target, label)
                                  : file_system.create_directories(*target);
  if (!placed) {
    return tl::unexpected(placed.error());
  }

  if (Result<void> written = write_all(
          *tools_, *changes, [](const Change& change) { return change.after; },
          [](const Change& change) { return change.before; });
      !written) {
    return tl::unexpected(written.error());
  }
  announce(*tools_, *changes);
  const Record record{.kind = relocation_.kind,
                      .source = location->path,
                      .target = *target,
                      .content = relocation_.content,
                      .changes = *std::move(changes)};
  return Undo{.items = {UndoItem{.item = item.id, .data = record_json(record)}}};
}

Result<Verification> RelocateAction::verify(const Undo& undo) {
  Result<Record> record = parse_record(undo);
  if (!record) {
    return tl::unexpected(record.error());
  }
  int holding = 0;
  for (const Change& change : record->changes) {
    Result<std::optional<EnvValue>> current = read_setting(*tools_, change);
    if (!current) {
      return tl::unexpected(current.error());
    }
    holding += *current == change.after ? 1 : 0;
  }
  const auto expected = static_cast<int>(record->changes.size());
  return Verification{.metric = "relocated_settings",
                      .before = static_cast<double>(expected),
                      .after = static_cast<double>(holding),
                      .effect = holding == expected ? Effect::better : Effect::worse};
}

Result<void> RelocateAction::undo(const Undo& undo) {
  Result<Record> record = parse_record(undo);
  if (!record) {
    return tl::unexpected(record.error());
  }
  for (const Change& change : record->changes) {
    Result<std::optional<EnvValue>> current = read_setting(*tools_, change);
    if (!current) {
      return tl::unexpected(current.error());
    }
    if (*current != change.after) {
      return fail(Errc::invalid_state,
                  change.name + " was changed since the relocation (now " + display(*current) +
                      ", relocation set " + display(change.after) +
                      "); nothing was undone, so that change is not overwritten");
    }
  }
  const std::vector<Change> reversed(record->changes.rbegin(), record->changes.rend());
  if (Result<void> written = write_all(
          *tools_, reversed, [](const Change& change) { return change.before; },
          [](const Change& change) { return change.after; });
      !written) {
    return written;
  }
  announce(*tools_, reversed);
  return {};
}

Result<Plan> leftover_plan(const os::IFileSystem& file_system, const Undo& undo,
                           const Actor& proposer, WallTime now) {
  Result<Record> record = parse_record(undo);
  if (!record) {
    return tl::unexpected(record.error());
  }
  Result<os::FileStat> stat = file_system.stat(record->source);
  if (!stat) {
    return tl::unexpected(stat.error());
  }
  Result<Bytes> size = readable_size(file_system, record->source);
  if (!size) {
    return tl::unexpected(size.error());
  }
  return Plan{
      .action = "QuarantineItems",
      .proposer = proposer,
      .created_at = now,
      .items = {PlanItem{.path = record->source, .identity = stat->identity, .size = *size}}};
}

}  // namespace myclean::placement
