#include "core/placement/recipe.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/cleanup/command_runner.hpp"
#include "core/model/action.hpp"
#include "core/model/enum_names.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "core/packs/schema.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/npmrc.hpp"
#include "core/placement/relocation.hpp"
#include "core/placement/settings.hpp"
#include "core/safety/paths.hpp"
#include "core/telemetry/process_table.hpp"

namespace myclean::placement {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using packs::RelocationRecipe;
using packs::SettingKind;

constexpr std::array<std::string_view, 3> registry_type_names = {"string", "expandable", "dword"};
constexpr std::string_view wsl_tool = "wsl";
constexpr std::string_view default_uid = "DefaultUid";  // the default user of a WSL distribution
constexpr std::string_view temporary_suffix = "-myclean-move";
constexpr std::string_view instance_wildcard = "\\*\\";
constexpr std::string_view target_placeholder = "{target}";
constexpr std::string_view name_placeholder = "{name}";
constexpr std::string_view device_prefix = R"(\\?\)";
constexpr std::chrono::hours move_timeout{3};  // a move copies a whole distribution disk
constexpr std::chrono::minutes query_timeout{1};

std::string utf8(const fs::path& path) {
  return safety::to_utf8(path);
}

fs::path from_utf8(std::string_view text) {
  return fs::path{std::u8string(text.begin(), text.end())};
}

// ASCII case-insensitive equality: image and instance names.
bool same_name(std::string_view left, std::string_view right) {
  return std::ranges::equal(left, right, [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  });
}

// What one run of the action works with.
struct Run {
  const Tools* tools;
  cleanup::ICommandRunner* runner;
  const std::stop_token* stop;
  const RelocationRecipe* recipe;
};

// Where the setting is.
struct Place {
  std::string instance;  // the instance's name as the registry holds it; empty without instances
  std::string key;       // registry: the key holding the value
  std::string name;      // registry: the value
  fs::path file;         // config: the file
  bool file_existed = false;  // config: whether the file existed when it was located
};

// The setting where it is, its stored value (nullopt: not set) and the folder it names.
struct Located {
  Place place;
  std::optional<RegistryValue> value;
  fs::path path;
};

// The undo data of one applied relocation.
struct Record {
  std::string recipe;
  Place place;
  fs::path source;
  fs::path target;
  std::optional<RegistryValue> before;  // env, config, registry
  std::optional<RegistryValue> after;
  fs::path archive;  // wsl-import: the archive left in the target
};

bool writes_setting(SettingKind kind) {
  return kind == SettingKind::env || kind == SettingKind::config || kind == SettingKind::registry;
}

// "HKCU\a\b\Value" as key "HKCU\a\b" and value "Value".
Place registry_place(std::string_view text) {
  const std::size_t slash = text.rfind('\\');
  return Place{.key = std::string(text.substr(0, slash)),
               .name = std::string(text.substr(slash + 1))};
}

// True when DWORD `value` passes `filter`.
bool kept(const std::optional<RegistryValue>& value, const packs::InstanceFilter& filter) {
  if (!value || value->type != RegistryType::dword) {
    return false;
  }
  std::uint32_t number = 0;
  const std::string& text = value->text;
  const char* last = std::to_address(text.end());
  const auto [end, error] = std::from_chars(text.data(), last, number);
  return error == std::errc{} && end == last && (number & filter.mask) == filter.equals;
}

// The instance of `current` (a registry value below a '*' key) whose value current.name is
// `instance` and that `where` keeps (every one when null).
Result<Place> find_instance(const IRegistry& registry, const packs::Setting& current,
                            std::string_view instance, const packs::InstanceFilter* where) {
  const std::size_t star = current.text.find(instance_wildcard);
  if (current.name.empty() || star == std::string::npos) {
    return fail(Errc::invalid_argument, "the setting " + current.text + " names no instances");
  }
  const std::string parent = current.text.substr(0, star);
  const std::string rest = current.text.substr(star + instance_wildcard.size());
  Result<std::vector<std::string>> subkeys = registry.subkeys(parent);
  if (!subkeys) {
    return tl::unexpected(subkeys.error());
  }
  for (const std::string& subkey : *subkeys) {
    std::string value_path = parent;
    value_path.append("\\").append(subkey).append("\\").append(rest);
    Place place = registry_place(value_path);
    Result<std::optional<RegistryValue>> name = registry.read(place.key, current.name);
    if (!name) {
      return tl::unexpected(name.error());
    }
    const std::optional<RegistryValue>& named = *name;
    if (!named || !same_name(named->text, instance)) {
      continue;
    }
    if (where != nullptr) {
      Result<std::optional<RegistryValue>> value = registry.read(place.key, where->value);
      if (!value) {
        return tl::unexpected(value.error());
      }
      if (!kept(*value, *where)) {
        continue;
      }
    }
    place.instance = named->text;
    return place;
  }
  return fail(Errc::not_found, "no instance \"" + std::string(instance) + "\" is registered");
}

std::string text_of(const cleanup::Command& command) {
  std::string text = command.program;
  for (const std::string& argument : command.arguments) {
    text += " " + argument;
  }
  return text;
}

// `word` with {target} and {name} replaced.
std::string substituted(std::string_view word, const std::string& target, std::string_view name) {
  std::string result;
  for (std::size_t at = 0; at < word.size();) {
    if (word.substr(at).starts_with(target_placeholder)) {
      result += target;
      at += target_placeholder.size();
    } else if (word.substr(at).starts_with(name_placeholder)) {
      result += name;
      at += name_placeholder.size();
    } else {
      result += word[at++];
    }
  }
  return result;
}

// `text` split at spaces, with the placeholders replaced inside each word: a folder with spaces
// stays one argument.
cleanup::Command command_of(std::string_view text, const fs::path& target, std::string_view name,
                            std::chrono::milliseconds timeout) {
  std::vector<std::string> words;
  for (std::size_t start = 0; start < text.size();) {
    const std::size_t end = std::min(text.find(' ', start), text.size());
    if (end > start) {
      words.push_back(substituted(text.substr(start, end - start), utf8(target), name));
    }
    start = end + 1;
  }
  cleanup::Command command{.program = words.empty() ? std::string{} : words.front(),
                           .arguments = {},
                           .timeout = timeout};
  if (!words.empty()) {
    command.arguments.assign(words.begin() + 1, words.end());
  }
  return command;
}

// Runs `command`; its standard output, or a failure for a non-zero exit code.
Result<std::string> run_tool(const Run& run, const cleanup::Command& command) {
  Result<cleanup::CommandOutput> output = run.runner->run(command, *run.stop);
  if (!output) {
    return fail(output.error().code,
                "`" + text_of(command) + "` failed: " + output.error().message);
  }
  if (output->exit_code != 0) {
    std::string said = output->err.empty() ? output->out : output->err;
    std::erase(said, '\0');  // wsl.exe writes UTF-16
    return fail(Errc::io_error, "`" + text_of(command) + "` exited with " +
                                    std::to_string(output->exit_code) + ": " + said);
  }
  return std::move(output->out);
}

Result<void> check_needs(const Run& run) {
  if (run.recipe->needs.empty()) {
    return {};
  }
  if (run.tools->processes == nullptr) {
    return fail(Errc::invalid_state, "the running programs cannot be listed here");
  }
  Result<std::vector<telemetry::ProcessInfo>> processes = run.tools->processes->snapshot();
  if (!processes) {
    return tl::unexpected(processes.error());
  }
  for (const telemetry::ProcessInfo& process : *processes) {
    for (const std::string& image : run.recipe->needs) {
      if (same_name(process.image, image)) {
        return fail(Errc::in_use,
                    image + " is running: quit it first, then the " + run.recipe->label + " moves");
      }
    }
  }
  return {};
}

// Where the setting of the recipe is for `instance`.
Result<Place> place_of(const Run& run, std::string_view instance) {
  const packs::Setting& current = run.recipe->current;
  if (current.kind == SettingKind::config) {
    Result<fs::path> file = run.tools->environment->expand(current.text);
    if (!file) {
      return tl::unexpected(file.error());
    }
    Result<std::optional<std::string>> text = run.tools->files->read(*file);
    if (!text) {
      return tl::unexpected(text.error());
    }
    return Place{.file = *std::move(file), .file_existed = text->has_value()};
  }
  if (current.kind != SettingKind::registry) {
    return Place{};
  }
  if (run.tools->registry == nullptr) {
    return fail(Errc::invalid_state, "the registry cannot be read here");
  }
  if (current.name.empty() && instance.empty()) {
    return registry_place(current.text);
  }
  if (instance.empty()) {
    return fail(Errc::invalid_argument, "the " + run.recipe->label + " needs an instance name");
  }
  if (std::ranges::any_of(run.recipe->skip_names, [&](const std::string& skipped) {
        return same_name(skipped, instance);
      })) {
    return fail(Errc::refused,
                "\"" + std::string(instance) + "\" is never moved as the " + run.recipe->label);
  }
  const packs::InstanceFilter* where = current.where ? &*current.where : nullptr;
  return find_instance(*run.tools->registry, current, instance, where);
}

// The value of configuration `text` (current.format, current.key); nullopt when it is not set.
Result<std::optional<RegistryValue>> config_value(const packs::Setting& current,
                                                  const std::string& text) {
  if (current.format == "ini") {
    std::optional<std::string> value = npmrc_value(text, current.key);
    return value ? std::optional<RegistryValue>{RegistryValue{.text = *std::move(value)}}
                 : std::nullopt;
  }
  const Json json = Json::parse(text, nullptr, false);
  if (json.is_discarded() || !json.is_object()) {
    return fail(Errc::invalid_data, current.text + " is not a JSON object");
  }
  const auto found = json.find(current.key);
  if (found == json.end()) {
    return std::optional<RegistryValue>{};
  }
  if (!found->is_string()) {
    return fail(Errc::invalid_data, current.key + " in " + current.text + " is not a string");
  }
  return std::optional<RegistryValue>{RegistryValue{.text = found->get<std::string>()}};
}

// The stored value of the setting at `place`; nullopt when it is not set.
Result<std::optional<RegistryValue>> read_value(const Run& run, const Place& place) {
  const packs::Setting& current = run.recipe->current;
  switch (current.kind) {
    case SettingKind::env: {
      Result<std::optional<EnvValue>> value = run.tools->environment->get(current.text);
      if (!value) {
        return tl::unexpected(value.error());
      }
      const std::optional<EnvValue>& variable = *value;
      if (!variable) {
        return std::optional<RegistryValue>{};
      }
      return std::optional<RegistryValue>{RegistryValue{
          .text = variable->text,
          .type = variable->expandable ? RegistryType::expandable : RegistryType::string}};
    }
    case SettingKind::config: {
      Result<std::optional<std::string>> text = run.tools->files->read(place.file);
      if (!text) {
        return tl::unexpected(text.error());
      }
      return *text ? config_value(current, **text) : std::optional<RegistryValue>{};
    }
    case SettingKind::registry:
      return run.tools->registry->read(place.key, place.name);
    case SettingKind::command: {
      Result<std::string> out = run_tool(run, command_of(current.text, {}, {}, query_timeout));
      if (!out) {
        return tl::unexpected(out.error());
      }
      const std::string line = out->substr(0, out->find_first_of("\r\n"));
      const std::size_t first = line.find_first_not_of(" \t");
      if (first == std::string::npos) {
        return std::optional<RegistryValue>{};
      }
      return std::optional<RegistryValue>{
          RegistryValue{.text = line.substr(first, line.find_last_not_of(" \t") - first + 1)}};
    }
    case SettingKind::wsl_import:
      break;
  }
  return fail(Errc::invalid_data, "wsl-import is no setting to read");
}

// The folder `value` names, or the recipe's default location when it is not set.
Result<fs::path> location_of(const Run& run, const std::optional<RegistryValue>& value) {
  const RelocationRecipe& recipe = *run.recipe;
  std::string text;
  if (value) {
    text = value->text;
  } else if (!recipe.locations.empty() &&
             recipe.locations.front().find_first_of("*?") == std::string::npos) {
    text = recipe.locations.front();
  } else {
    return fail(Errc::not_found, "the " + recipe.label + " is not set and has no default location");
  }
  if (text.starts_with(device_prefix)) {
    text.erase(0, device_prefix.size());
  }
  Result<fs::path> expanded = run.tools->environment->expand(text);
  Result<fs::path> path = expanded ? safety::normalize_path(*expanded) : expanded;
  if (!path) {
    return fail(path.error().code, "the " + recipe.label + " location \"" + text +
                                       "\" is not usable: " + path.error().message);
  }
  return path;
}

Result<Located> locate(const Run& run, std::string_view instance) {
  Result<Place> place = place_of(run, instance);
  if (!place) {
    return tl::unexpected(place.error());
  }
  Result<std::optional<RegistryValue>> value = read_value(run, *place);
  if (!value) {
    return tl::unexpected(value.error());
  }
  Result<fs::path> path = location_of(run, *value);
  if (!path) {
    return tl::unexpected(path.error());
  }
  return Located{.place = *std::move(place), .value = *std::move(value), .path = *std::move(path)};
}

Result<void> write_config(const Run& run, const Place& place,
                          const std::optional<RegistryValue>& value) {
  const packs::Setting& current = run.recipe->current;
  Result<std::optional<std::string>> text = run.tools->files->read(place.file);
  if (!text) {
    return tl::unexpected(text.error());
  }
  const std::string old = text->value_or("");
  std::string updated;
  bool empty = false;
  if (current.format == "ini") {
    updated = value ? npmrc_set(old, current.key, value->text) : npmrc_remove(old, current.key);
    empty = updated.find_first_not_of(" \t\r\n") == std::string::npos;
  } else {
    nlohmann::ordered_json json = old.find_first_not_of(" \t\r\n") == std::string::npos
                                      ? nlohmann::ordered_json::object()
                                      : nlohmann::ordered_json::parse(old, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      return fail(Errc::invalid_data, current.text + " is not a JSON object");
    }
    if (value) {
      json[current.key] = value->text;
    } else {
      json.erase(current.key);
    }
    empty = json.empty();
    updated = json.dump(2) + "\n";
  }
  if (!value && !place.file_existed && empty) {
    return run.tools->files->remove(place.file);  // the file MyClean created goes away again
  }
  return run.tools->files->write(place.file, updated);
}

// Writes `value` to the setting at `place`, deleting it for nullopt.
Result<void> write_value(const Run& run, const Place& place,
                         const std::optional<RegistryValue>& value) {
  const packs::Setting& current = run.recipe->current;
  switch (current.kind) {
    case SettingKind::env:
      return value ? run.tools->environment->set(
                         current.text,
                         EnvValue{.text = value->text,
                                  .expandable = value->type == RegistryType::expandable})
                   : run.tools->environment->remove(current.text);
    case SettingKind::config:
      return write_config(run, place, value);
    case SettingKind::registry:
      return value ? run.tools->registry->write(place.key, place.name, *value)
                   : run.tools->registry->remove(place.key, place.name);
    case SettingKind::command:
    case SettingKind::wsl_import:
      break;
  }
  return fail(Errc::invalid_data, "a command is no setting to write");
}

// Whether the folder the copy at `place` names is `folder` and the copy starts.
Result<void> check_copy(const Run& run, const Place& place, const fs::path& folder) {
  Result<std::optional<RegistryValue>> value = read_value(run, place);
  if (!value) {
    return tl::unexpected(value.error());
  }
  Result<fs::path> path = location_of(run, *value);
  if (!path) {
    return tl::unexpected(path.error());
  }
  if (!same_path(*path, folder)) {
    return fail(Errc::io_error, "it is registered at " + utf8(*path));
  }
  Result<std::string> started =
      run_tool(run, cleanup::Command{.program = std::string(wsl_tool),
                                     .arguments = {"-d", place.instance, "-e", "true"},
                                     .timeout = query_timeout});
  return started ? Result<void>{} : tl::unexpected(started.error());
}

// The WSL 1 move of wsl-import (see the header): distribution `place` to folder `to`. The
// archive it leaves there.
Result<fs::path> import_to(const Run& run, const Place& place, const fs::path& to) {
  const packs::Setting& current = run.recipe->current;
  IRegistry& registry = *run.tools->registry;
  const std::string& name = place.instance;
  const std::string temporary = name + std::string(temporary_suffix);
  const Result<Place> clash = find_instance(registry, current, temporary, nullptr);
  if (clash) {
    return fail(Errc::already_exists,
                "a distribution named " + temporary + " exists already, so nothing changed");
  }
  if (clash.error().code != Errc::not_found) {
    return tl::unexpected(clash.error());
  }
  Result<std::optional<RegistryValue>> uid = registry.read(place.key, default_uid);
  if (!uid) {
    return tl::unexpected(uid.error());
  }
  if (Result<void> created = run.tools->file_system->create_directories(to); !created) {
    return tl::unexpected(created.error());
  }
  fs::path archive = to / from_utf8(name + ".tar");
  const auto wsl = [&](std::vector<std::string> arguments) {
    return run_tool(run, cleanup::Command{.program = std::string(wsl_tool),
                                          .arguments = std::move(arguments),
                                          .timeout = move_timeout});
  };
  if (Result<std::string> exported = wsl({"--export", name, utf8(archive)}); !exported) {
    return fail(exported.error().code, exported.error().message + "; nothing changed");
  }
  if (Result<std::string> imported =
          wsl({"--import", temporary, utf8(to), utf8(archive), "--version", "1"});
      !imported) {
    return fail(imported.error().code, imported.error().message + "; " + name +
                                           " stays as it was, its archive is " + utf8(archive));
  }
  Result<Place> copy = find_instance(registry, current, temporary, nullptr);
  const Result<void> checked = copy ? check_copy(run, *copy, to) : tl::unexpected(copy.error());
  if (!checked) {
    static_cast<void>(wsl({"--unregister", temporary}));
    return fail(Errc::io_error, "the import at " + utf8(to) + " could not be checked (" +
                                    checked.error().message + "), so it was removed and " + name +
                                    " stays as it was");
  }
  if (Result<std::string> removed = wsl({"--unregister", name}); !removed) {
    static_cast<void>(wsl({"--unregister", temporary}));
    return fail(removed.error().code, removed.error().message + "; the copy was removed and " +
                                          name + " stays as it was");
  }
  // The copy takes the original's place: its name and default user.
  Result<void> renamed = registry.write(copy->key, current.name, RegistryValue{.text = name});
  if (renamed && *uid) {
    renamed = registry.write(copy->key, default_uid, **uid);
  }
  if (!renamed) {
    return fail(renamed.error().code, name + " moved to " + utf8(to) +
                                          " but is still registered as " + temporary + ": " +
                                          renamed.error().message);
  }
  return archive;
}

Json value_json(const std::optional<RegistryValue>& value) {
  if (!value) {
    return nullptr;
  }
  return Json{{"text", value->text}, {"type", detail::enum_name(registry_type_names, value->type)}};
}

std::optional<RegistryValue> value_from(const Json& json) {
  if (json.is_null()) {
    return std::nullopt;
  }
  const std::optional<RegistryType> type =
      detail::enum_from_name<RegistryType>(registry_type_names, json.at("type").get<std::string>());
  if (!type) {
    throw std::invalid_argument("unknown value type");
  }
  return RegistryValue{.text = json.at("text").get<std::string>(), .type = *type};
}

std::string record_json(const Record& record) {
  return Json{{"recipe", record.recipe},
              {"instance", record.place.instance},
              {"key", record.place.key},
              {"name", record.place.name},
              {"file", utf8(record.place.file)},
              {"file_existed", record.place.file_existed},
              {"source", utf8(record.source)},
              {"target", utf8(record.target)},
              {"before", value_json(record.before)},
              {"after", value_json(record.after)},
              {"archive", utf8(record.archive)}}
      .dump();
}

Result<Record> parse_record(const Undo& undo, const RelocationRecipe& recipe) {
  if (undo.items.size() != 1) {
    return fail(Errc::invalid_data, "relocation undo data must hold exactly one item");
  }
  try {
    const Json json = Json::parse(undo.items.front().data);
    Record record{.recipe = json.at("recipe").get<std::string>(),
                  .place = {.instance = json.at("instance").get<std::string>(),
                            .key = json.at("key").get<std::string>(),
                            .name = json.at("name").get<std::string>(),
                            .file = from_utf8(json.at("file").get<std::string>()),
                            .file_existed = json.at("file_existed").get<bool>()},
                  .source = from_utf8(json.at("source").get<std::string>()),
                  .target = from_utf8(json.at("target").get<std::string>()),
                  .before = value_from(json.at("before")),
                  .after = value_from(json.at("after")),
                  .archive = from_utf8(json.at("archive").get<std::string>())};
    if (record.recipe != recipe.id) {
      return fail(Errc::invalid_data,
                  "the undo data is of recipe " + record.recipe + ", not " + recipe.id);
    }
    return record;
  } catch (const std::exception& error) {
    return fail(Errc::invalid_data,
                std::string("relocation undo data is malformed: ") + error.what());
  }
}

std::string display(const std::optional<RegistryValue>& value) {
  return value ? "\"" + value->text + "\"" : "not set";
}

// Moves what `place` names from `source` to `target` with the recipe's tool (command or
// wsl-import); the archive a wsl-import leaves.
Result<fs::path> move_with_tool(const Run& run, const Place& place, const fs::path& target) {
  if (run.recipe->target_kind == SettingKind::wsl_import) {
    return import_to(run, place, target);
  }
  Result<std::string> moved =
      run_tool(run, command_of(run.recipe->command, target, place.instance, move_timeout));
  return moved ? Result<fs::path>{fs::path{}} : tl::unexpected(moved.error());
}

// `requested` normalized, once it neither overlaps the location nor lies on a drive that is
// unhealthy or lacks room, and nothing the recipe needs runs.
Result<fs::path> checked_target(const Run& run, const Located& located, const fs::path& requested) {
  Result<fs::path> target = safety::normalize_path(requested);
  if (!target) {
    return target;
  }
  if (same_path(*target, located.path) || inside(*target, located.path) ||
      inside(located.path, *target)) {
    return fail(Errc::invalid_argument,
                "the target " + utf8(*target) + " overlaps the current folder");
  }
  if (Result<void> idle = check_needs(run); !idle) {
    return tl::unexpected(idle.error());
  }
  Result<Bytes> size = readable_size(*run.tools->file_system, located.path);
  if (!size) {
    return tl::unexpected(size.error());
  }
  Bytes needed = *size;
  if (run.recipe->target_kind == SettingKind::wsl_import) {
    needed += *size;  // the archive and the imported files at once
  }
  // The target's drive is checked now, not when it was proposed: it may have degraded since.
  if (Result<void> safe = check_target(*run.tools->drives, *target, needed); !safe) {
    return tl::unexpected(safe.error());
  }
  return target;
}

// Places the content (`content`) at record.target, then writes it into the setting; the
// setting's values go to `record`.
Result<void> switch_setting(const Run& run, ContentMode content, const Located& located,
                            Record& record) {
  os::IFileSystem& file_system = *run.tools->file_system;
  const Result<os::FileStat> source = file_system.stat(located.path);
  if (!source && source.error().code != Errc::not_found) {
    return tl::unexpected(source.error());
  }
  Result<void> placed =
      content == ContentMode::move && source
          ? copy_content(*run.tools, located.path, record.target, run.recipe->label)
          : file_system.create_directories(record.target);
  if (!placed) {
    return placed;
  }
  const std::optional<RegistryValue>& before = located.value;
  const bool expandable = before && before->type == RegistryType::expandable;
  record.before = before;
  record.after =
      RegistryValue{.text = utf8(record.target),
                    .type = expandable ? RegistryType::expandable : RegistryType::string};
  if (Result<void> written = write_value(run, record.place, record.after); !written) {
    return fail(written.error().code,
                "writing " + run.recipe->current.text + " failed: " + written.error().message);
  }
  if (run.recipe->current.kind == SettingKind::env) {
    static_cast<void>(run.tools->environment->announce());  // best effort, as RelocateAction
  }
  return {};
}

// Moves the content to record.target with the recipe's tool; a wsl-import starts only in an
// empty or missing folder.
Result<void> move_to_target(const Run& run, const Located& located, Record& record) {
  if (run.recipe->target_kind == SettingKind::wsl_import) {
    if (Result<std::vector<os::DirEntry>> entries =
            run.tools->file_system->enumerate(record.target);
        entries && !entries->empty()) {
      return fail(Errc::not_empty, "the target " + utf8(record.target) + " is not an empty folder");
    }
  }
  Result<fs::path> archive = move_with_tool(run, located.place, record.target);
  if (!archive) {
    return tl::unexpected(archive.error());
  }
  record.archive = *std::move(archive);
  return {};
}

}  // namespace

Json recipe_json(const RelocationRecipe& recipe) {
  const packs::Setting& current = recipe.current;
  Json where = nullptr;
  if (current.where) {
    where = Json{{"value", current.where->value},
                 {"mask", current.where->mask},
                 {"equals", current.where->equals}};
  }
  return Json{{"id", recipe.id},
              {"label", recipe.label},
              {"locations", recipe.locations},
              {"current",
               {{"kind", packs::to_string(current.kind)},
                {"text", current.text},
                {"key", current.key},
                {"format", current.format},
                {"name", current.name},
                {"where", std::move(where)}}},
              {"target_kind", packs::to_string(recipe.target_kind)},
              {"command", recipe.command},
              {"content", packs::to_string(recipe.content)},
              {"needs", recipe.needs},
              {"skip_names", recipe.skip_names},
              {"undo", packs::to_string(recipe.undo)},
              {"note", recipe.note}};
}

Result<RelocationRecipe> recipe_from_json(const Json& json) {
  try {
    const Json& current = json.at("current");
    const std::optional<SettingKind> kind =
        packs::parse_setting_kind(current.at("kind").get<std::string>());
    const std::optional<SettingKind> target_kind =
        packs::parse_setting_kind(json.at("target_kind").get<std::string>());
    const std::optional<packs::ContentMode> content =
        packs::parse_content_mode(json.at("content").get<std::string>());
    const std::optional<packs::UndoKind> undo =
        packs::parse_undo_kind(json.at("undo").get<std::string>());
    if (!kind || !target_kind || !content || !undo) {
      return fail(Errc::invalid_data, "a relocation recipe names an unknown kind or mode");
    }
    RelocationRecipe recipe{.id = json.at("id").get<std::string>(),
                            .label = json.at("label").get<std::string>(),
                            .locations = json.at("locations").get<std::vector<std::string>>(),
                            .current = {.kind = *kind,
                                        .text = current.at("text").get<std::string>(),
                                        .key = current.at("key").get<std::string>(),
                                        .format = current.at("format").get<std::string>(),
                                        .name = current.at("name").get<std::string>(),
                                        .where = std::nullopt},
                            .target_kind = *target_kind,
                            .command = json.at("command").get<std::string>(),
                            .content = *content,
                            .needs = json.at("needs").get<std::vector<std::string>>(),
                            .skip_names = json.at("skip_names").get<std::vector<std::string>>(),
                            .undo = *undo,
                            .note = json.at("note").get<std::string>()};
    if (const Json& where = current.at("where"); !where.is_null()) {
      recipe.current.where =
          packs::InstanceFilter{.value = where.at("value").get<std::string>(),
                                .mask = where.at("mask").get<std::uint32_t>(),
                                .equals = where.at("equals").get<std::uint32_t>()};
    }
    return recipe;
  } catch (const Json::exception& error) {
    return fail(Errc::invalid_data,
                std::string("a relocation recipe is malformed: ") + error.what());
  }
}

std::vector<std::string> recipe_commands(const RecipeRelocation& relocation) {
  const RelocationRecipe& recipe = relocation.recipe;
  const std::string& name = relocation.instance;
  const std::string target = utf8(relocation.target);
  if (recipe.target_kind == SettingKind::command) {
    return {text_of(command_of(recipe.command, relocation.target, name, move_timeout))};
  }
  if (recipe.target_kind == SettingKind::wsl_import) {
    const std::string temporary = name + std::string(temporary_suffix);
    const std::string archive = utf8(relocation.target / from_utf8(name + ".tar"));
    return {"wsl --export " + name + " " + archive,
            "wsl --import " + temporary + " " + target + " " + archive + " --version 1",
            "wsl -d " + temporary + " -e true",
            "wsl --unregister " + name + " (only after the import is checked)",
            "set " + recipe.current.name + " and " + std::string(default_uid) + " of " + temporary +
                " to the original's"};
  }
  return {std::string(relocation.content == ContentMode::move
                          ? "copy the content and check the copy, then set "
                          : "set ") +
          recipe.current.text + " to " + target};
}

ActionInfo RecipeAction::describe() const {
  const RelocationRecipe& recipe = relocation_.recipe;
  std::string what = "Relocate the " + recipe.label;
  if (!relocation_.instance.empty()) {
    what += " \"" + relocation_.instance + "\"";
  }
  what += " to " + utf8(relocation_.target);
  std::string why;
  switch (recipe.target_kind) {
    case SettingKind::command:
      why = "it moves with the tool's own command `" + recipe.command + "`";
      break;
    case SettingKind::wsl_import:
      why =
          "WSL 1 cannot move it in place: it is exported, imported at the target and checked "
          "before the original is unregistered";
      break;
    default:
      why = relocation_.content == ContentMode::move
                ? "its content is copied and checked before " + recipe.current.text + " changes"
                : "the tool rebuilds its content at the new location after " + recipe.current.text +
                      " changes";
      break;
  }
  return {.what = std::move(what),
          .why = std::move(why),
          .risk = recipe.target_kind == SettingKind::wsl_import ? Risk::high : Risk::medium,
          .tier = Tier::ask,
          .context = ExecutionContext::user};
}

Result<Preview> RecipeAction::preview() {
  const Run run{.tools = tools_, .runner = runner_, .stop = &stop_, .recipe = &relocation_.recipe};
  Result<Located> located = locate(run, relocation_.instance);
  if (!located) {
    return tl::unexpected(located.error());
  }
  Result<Bytes> size = readable_size(*tools_->file_system, located->path);
  if (!size) {
    return tl::unexpected(size.error());
  }
  PlanItem item{.path = located->path, .size = *size};
  if (const Result<os::FileStat> stat = tools_->file_system->stat(located->path); stat) {
    item.identity = stat->identity;
  } else if (stat.error().code != Errc::not_found) {
    return tl::unexpected(stat.error());
  }
  return Preview{.items = {std::move(item)}};
}

Result<Undo> RecipeAction::apply(const Preview& preview) {
  if (preview.items.size() != 1) {
    return fail(Errc::invalid_argument, "a relocation preview holds exactly one item");
  }
  const PlanItem& item = preview.items.front();
  const RelocationRecipe& recipe = relocation_.recipe;
  const Run run{.tools = tools_, .runner = runner_, .stop = &stop_, .recipe = &recipe};
  Result<Located> located = locate(run, relocation_.instance);
  if (!located) {
    return tl::unexpected(located.error());
  }
  if (!same_path(located->path, item.path)) {
    return fail(Errc::refused, "the " + recipe.label + " moved since the preview: it is now at " +
                                   utf8(located->path));
  }
  Result<fs::path> target = checked_target(run, *located, relocation_.target);
  if (!target) {
    return tl::unexpected(target.error());
  }
  Record record{
      .recipe = recipe.id, .place = located->place, .source = located->path, .target = *target};
  if (Result<void> moved = writes_setting(recipe.target_kind)
                               ? switch_setting(run, relocation_.content, *located, record)
                               : move_to_target(run, *located, record);
      !moved) {
    return tl::unexpected(moved.error());
  }
  Undo undo{.items = {UndoItem{.item = item.id, .data = record_json(record)}}};
  if (recipe.undo == packs::UndoKind::none) {
    undo.not_reversible = recipe.note;
  }
  return undo;
}

Result<Verification> RecipeAction::verify(const Undo& undo) {
  const RelocationRecipe& recipe = relocation_.recipe;
  Result<Record> record = parse_record(undo, recipe);
  if (!record) {
    return tl::unexpected(record.error());
  }
  const Run run{.tools = tools_, .runner = runner_, .stop = &stop_, .recipe = &recipe};
  bool holds = false;
  if (writes_setting(recipe.target_kind)) {
    Result<std::optional<RegistryValue>> value = read_value(run, record->place);
    if (!value) {
      return tl::unexpected(value.error());
    }
    holds = *value == record->after;
  } else {
    Result<Located> located = locate(run, record->place.instance);
    if (!located) {
      return tl::unexpected(located.error());
    }
    holds = same_path(located->path, record->target);
  }
  return Verification{.metric = "relocated",
                      .before = 0.0,
                      .after = holds ? 1.0 : 0.0,
                      .effect = holds ? Effect::better : Effect::worse};
}

Result<void> RecipeAction::undo(const Undo& undo) {
  const RelocationRecipe& recipe = relocation_.recipe;
  if (recipe.undo == packs::UndoKind::none) {
    return fail(Errc::not_reversible, "not reversible: " + recipe.note);
  }
  Result<Record> record = parse_record(undo, recipe);
  if (!record) {
    return tl::unexpected(record.error());
  }
  const Run run{.tools = tools_, .runner = runner_, .stop = &stop_, .recipe = &recipe};
  if (writes_setting(recipe.target_kind)) {
    Result<std::optional<RegistryValue>> value = read_value(run, record->place);
    if (!value) {
      return tl::unexpected(value.error());
    }
    if (*value != record->after) {
      return fail(Errc::invalid_state,
                  recipe.current.text + " was changed since the relocation (now " +
                      display(*value) + ", relocation set " + display(record->after) +
                      "); nothing was undone, so that change is not overwritten");
    }
    if (Result<void> written = write_value(run, record->place, record->before); !written) {
      return written;
    }
    if (recipe.current.kind == SettingKind::env) {
      static_cast<void>(tools_->environment->announce());
    }
    return {};
  }
  Result<Located> located = locate(run, record->place.instance);
  if (!located) {
    return tl::unexpected(located.error());
  }
  if (!same_path(located->path, record->target)) {
    return fail(Errc::invalid_state, "the " + recipe.label + " is now at " + utf8(located->path) +
                                         ", not where the relocation put it (" +
                                         utf8(record->target) + "); nothing was undone");
  }
  if (Result<void> idle = check_needs(run); !idle) {
    return idle;
  }
  Result<fs::path> moved = move_with_tool(run, located->place, record->source);
  return moved ? Result<void>{} : tl::unexpected(moved.error());
}

}  // namespace myclean::placement
