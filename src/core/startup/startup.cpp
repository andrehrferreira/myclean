#include "core/startup/startup.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::startup {
namespace {

constexpr std::array<std::pair<Source, std::string_view>, 7> source_names{{
    {Source::run, "run"},
    {Source::run32, "run32"},
    {Source::run_once, "runonce"},
    {Source::run_once32, "runonce32"},
    {Source::startup_folder, "folder"},
    {Source::task, "task"},
    {Source::service, "service"},
}};

constexpr std::array<std::pair<StartType, std::string_view>, 5> start_type_names{{
    {StartType::boot, "boot"},
    {StartType::system, "system"},
    {StartType::automatic, "automatic"},
    {StartType::manual, "manual"},
    {StartType::disabled, "disabled"},
}};

template <typename Enum, std::size_t N>
std::string_view name_of(const std::array<std::pair<Enum, std::string_view>, N>& names,
                         Enum value) {
  for (const auto& [candidate, name] : names) {
    if (candidate == value) {
      return name;
    }
  }
  return {};
}

template <typename Enum, std::size_t N>
std::optional<Enum> value_of(const std::array<std::pair<Enum, std::string_view>, N>& names,
                             std::string_view text) {
  for (const auto& [value, name] : names) {
    if (name == text) {
      return value;
    }
  }
  return std::nullopt;
}

// 100-ns intervals between 1601-01-01 (FILETIME) and 1970-01-01 (system_clock).
constexpr std::int64_t filetime_epoch_offset = 116444736000000000;

bool ascii_iequal(std::string_view a, std::string_view b) {
  return std::ranges::equal(a, b, [](char x, char y) {
    const auto lower = [](char c) {
      return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    return lower(x) == lower(y);
  });
}

bool starts_with_iequal(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && ascii_iequal(text.substr(0, prefix.size()), prefix);
}

Source run_source(const RunValue& value) {
  if (value.once) {
    return value.wow64 ? Source::run_once32 : Source::run_once;
  }
  return value.wow64 ? Source::run32 : Source::run;
}

// The StartupApproved subkey of a toggleable Run or folder source.
std::optional<ApprovedKey> approved_key(Source source) {
  switch (source) {
    case Source::run:
      return ApprovedKey::run;
    case Source::run32:
      return ApprovedKey::run32;
    case Source::startup_folder:
      return ApprovedKey::startup_folder;
    default:
      return std::nullopt;
  }
}

using ApprovedIndex = std::map<std::tuple<Scope, ApprovedKey, std::string>, const Approved*>;

Item registry_item(Source source, Scope scope, std::string name, std::string command,
                   const ApprovedIndex& approved) {
  Item item{.id = item_id(source, scope, name),
            .source = source,
            .scope = scope,
            .name = name,
            .display_name = name,
            .command = std::move(command)};
  if (const std::optional<ApprovedKey> key = approved_key(source)) {
    const auto found = approved.find({scope, *key, std::move(name)});
    if (found != approved.end()) {
      item.approved = found->second->bytes;
    }
    item.enabled = !approved_disabled(item.approved);
  }
  return item;
}

bool windows_task(const Task& task) {
  if (task.signer) {
    return *task.signer == windows_signer;
  }
  return task.command.empty() && starts_with_iequal(task.path, R"(\Microsoft\Windows\)");
}

}  // namespace

std::string_view to_string(Source source) {
  return name_of(source_names, source);
}
std::optional<Source> parse_source(std::string_view text) {
  return value_of(source_names, text);
}

std::string_view to_string(Scope scope) {
  return scope == Scope::user ? "user" : "machine";
}

std::optional<Scope> parse_scope(std::string_view text) {
  if (text == "user") {
    return Scope::user;
  }
  if (text == "machine") {
    return Scope::machine;
  }
  return std::nullopt;
}

bool approved_disabled(const ApprovedValue& value) {
  return value && !value->empty() && ((*value)[0] & 1U) != 0;
}

std::vector<std::uint8_t> approved_bytes(bool disabled, WallTime when) {
  std::vector<std::uint8_t> bytes(12, 0);
  bytes[0] = disabled ? 3 : 2;
  if (disabled) {
    const auto ticks =
        std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>>(
            when.time_since_epoch())
            .count() +
        filetime_epoch_offset;
    auto filetime = static_cast<std::uint64_t>(ticks);
    for (std::size_t index = 4; index < bytes.size(); ++index) {
      bytes[index] = static_cast<std::uint8_t>(filetime & 0xFFU);
      filetime >>= 8U;
    }
  }
  return bytes;
}

std::string_view to_string(StartType type) {
  return name_of(start_type_names, type);
}

std::optional<StartType> parse_start_type(std::string_view text) {
  return value_of(start_type_names, text);
}

std::string item_id(Source source, Scope scope, std::string_view name) {
  std::string id(to_string(source));
  id += ':';
  id += to_string(scope);
  id += ':';
  id += name;
  return id;
}

Result<ItemKey> parse_item_id(std::string_view id) {
  const std::size_t first = id.find(':');
  const std::size_t second = first == std::string_view::npos ? first : id.find(':', first + 1);
  if (second == std::string_view::npos || second + 1 == id.size()) {
    return fail(Errc::invalid_argument, "not a startup item id: " + std::string(id));
  }
  const std::optional<Source> source = parse_source(id.substr(0, first));
  const std::optional<Scope> scope = parse_scope(id.substr(first + 1, second - first - 1));
  if (!source || !scope) {
    return fail(Errc::invalid_argument, "not a startup item id: " + std::string(id));
  }
  return ItemKey{.source = *source, .scope = *scope, .name = std::string(id.substr(second + 1))};
}

std::vector<Item> inventory(const Snapshot& snapshot) {
  ApprovedIndex approved;
  for (const Approved& value : snapshot.approved) {
    approved.emplace(std::tuple{value.scope, value.key, value.name}, &value);
  }
  std::vector<Item> items;
  items.reserve(snapshot.run.size() + snapshot.files.size() + snapshot.tasks.size() +
                snapshot.services.size());
  for (const RunValue& value : snapshot.run) {
    items.push_back(
        registry_item(run_source(value), value.scope, value.name, value.command, approved));
  }
  for (const StartupFile& file : snapshot.files) {
    if (ascii_iequal(file.file_name, "desktop.ini")) {
      continue;
    }
    items.push_back(
        registry_item(Source::startup_folder, file.scope, file.file_name, file.target, approved));
  }
  for (const Task& task : snapshot.tasks) {
    if ((!task.logon && !task.boot) || windows_task(task)) {
      continue;
    }
    items.push_back(Item{.id = item_id(Source::task, Scope::machine, task.path),
                         .source = Source::task,
                         .scope = Scope::machine,
                         .name = task.path,
                         .display_name = task.path,
                         .command = task.command,
                         .enabled = task.enabled,
                         .signer = task.signer});
  }
  for (const Service& service : snapshot.services) {
    if (service.start.type != StartType::automatic || service.signer == windows_signer) {
      continue;
    }
    items.push_back(Item{.id = item_id(Source::service, Scope::machine, service.name),
                         .source = Source::service,
                         .scope = Scope::machine,
                         .name = service.name,
                         .display_name = service.display_name,
                         .command = service.binary,
                         .enabled = true,
                         .start = service.start,
                         .signer = service.signer});
  }
  std::ranges::stable_sort(items, [](const Item& a, const Item& b) {
    return std::tie(a.source, a.scope, a.name) < std::tie(b.source, b.scope, b.name);
  });
  return items;
}

const Item* find_item(std::span<const Item> items, std::string_view id) {
  const auto found = std::ranges::find(items, id, &Item::id);
  return found == items.end() ? nullptr : &*found;
}

bool toggleable(const Item& item) {
  return item.source != Source::run_once && item.source != Source::run_once32;
}

}  // namespace myclean::startup
