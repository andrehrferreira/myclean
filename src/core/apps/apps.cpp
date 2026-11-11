#include "core/apps/apps.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/text.hpp"
#include "core/model/units.hpp"
#include "core/placement/settings.hpp"
#include "core/rules/schema.hpp"
#include "core/scanall/owner_decisions.hpp"
#include "core/store/database.hpp"
#include "core/store/store.hpp"
#include "core/trust/trust.hpp"

namespace myclean::apps {

namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

constexpr std::uint64_t kibibyte = 1024;

std::string lower(std::string_view text) {
  std::string folded(text);
  std::ranges::transform(folded, folded.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return folded;
}

bool contains(std::string_view text, std::string_view part) {
  return lower(text).find(lower(part)) != std::string::npos;
}

fs::path from_utf8(std::string_view text) {
  return fs::path{std::u8string(text.begin(), text.end())};
}

std::string trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '"')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '"')) {
    text.remove_suffix(1);
  }
  return std::string(text);
}

std::string read_text(const placement::IRegistry& registry, const std::string& key,
                      std::string_view name) {
  const Result<std::optional<placement::RegistryValue>> value = registry.read(key, name);
  return value ? value->value_or(placement::RegistryValue{}).text : std::string{};
}

std::uint64_t read_number(const placement::IRegistry& registry, const std::string& key,
                          std::string_view name) {
  const std::string text = read_text(registry, key, name);
  std::uint64_t number = 0;
  for (const char c : text) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
      return 0;
    }
    number = number * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return number;
}

// "20240302" as "2024-03-02"; empty when it is no date.
std::string install_date(std::string_view text) {
  if (text.size() != 8 || !std::ranges::all_of(text, [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
      })) {
    return {};
  }
  return std::string(text.substr(0, 4)) + "-" + std::string(text.substr(4, 2)) + "-" +
         std::string(text.substr(6, 2));
}

std::string date_text(WallTime time) {
  return std::format("{:%Y-%m-%d}", std::chrono::floor<std::chrono::days>(time));
}

// The program of an UninstallString or DisplayIcon: the quoted first token, the text up to
// ".exe", or the text before ",<index>".
std::string program_of(std::string_view command) {
  const std::string text = trim(command);
  if (!command.empty() && command.front() == '"') {
    const std::size_t close = command.find('"', 1);
    return close == std::string_view::npos ? text : std::string(command.substr(1, close - 1));
  }
  const std::string folded = lower(text);
  if (const std::size_t exe = folded.find(".exe"); exe != std::string::npos) {
    return text.substr(0, exe + 4);
  }
  return text.substr(0, text.find(','));
}

// A folder an app's own programs do not run from: Windows, the Installer cache, Package Cache.
bool foreign_folder(const fs::path& folder) {
  const std::string text = lower(utf8_text(folder));
  return text.empty() || folder == folder.root_path() || text.find(":\\windows") == 1 ||
         text.find("\\installer\\") != std::string::npos || text.ends_with("\\installer") ||
         text.find("\\package cache") != std::string::npos;
}

std::string with_separator(std::string text) {
  if (!text.empty() && text.back() != '\\') {
    text.push_back('\\');
  }
  return text;
}

// A run with its path folded to lower case once.
struct Run {
  std::string folded;
  const std::string* path = nullptr;
  UsageSource source = UsageSource::user_assist;
  WallTime time;
};

// The runs of `records` and `history`, folded.
std::vector<Run> runs_of(std::span<const RunRecord> records, const ProcessHistory& history) {
  std::vector<Run> runs;
  runs.reserve(records.size() + history.last().size());
  for (const RunRecord& record : records) {
    runs.push_back(Run{.folded = lower(record.path),
                       .path = &record.path,
                       .source = record.source,
                       .time = record.last_run});
  }
  for (const auto& [image, time] : history.last()) {
    runs.push_back(
        Run{.folded = image, .path = &image, .source = UsageSource::myclean, .time = time});
  }
  return runs;
}

// What a run's folded path is compared with: the app's roots with a trailing separator and its
// Appx family name, folded.
class Matcher {
 public:
  explicit Matcher(const App& app) : family_(lower(app.family)) {
    for (const fs::path& root : app.roots) {
      roots_.push_back(with_separator(lower(utf8_text(root))));
    }
  }

  // Whether the folded run path `folded` is a program of the app, or its Appx app id.
  [[nodiscard]] bool matches(std::string_view folded) const {
    if (!family_.empty() && folded.starts_with(family_) &&
        (folded.size() == family_.size() || folded[family_.size()] == '!')) {
      return true;
    }
    return std::ranges::any_of(roots_,
                               [&](const std::string& root) { return folded.starts_with(root); });
  }

 private:
  std::string family_;
  std::vector<std::string> roots_;
};

std::string source_text(UsageSource source) {
  return std::string(to_string(source));
}

LastUsed newest_run(const Matcher& matcher, std::span<const Run> runs,
                    const ProcessHistory& history, WallTime now) {
  const bool long_history =
      history.since() && now - *history.since() >= std::chrono::days{authoritative_history};
  LastUsed used{.confidence = long_history ? Confidence::high : Confidence::low};
  for (const Run& run : runs) {
    if ((used.time && *used.time >= run.time) || !matcher.matches(run.folded)) {
      continue;
    }
    used.time = run.time;
    used.source = run.source;
    if (run.source == UsageSource::user_assist) {
      used.confidence = Confidence::medium;
    } else if (run.source == UsageSource::myclean) {
      used.confidence = long_history ? Confidence::high : Confidence::medium;
    } else {
      used.confidence = Confidence::high;
    }
  }
  return used;
}

// The knowledge-base entry of the first of `icon` and the programs of `runs` that are the app's.
const rules::KbEntry* kb_entry(const App& app, const Matcher& matcher, const fs::path& icon,
                               std::span<const Run> runs, std::span<const rules::KbEntry> kb) {
  if (kb.empty()) {
    return nullptr;
  }
  const std::optional<std::string> signer =
      app.publisher.empty() ? std::nullopt : std::optional(app.publisher);
  if (lower(utf8_text(icon)).ends_with(".exe")) {
    if (const rules::KbEntry* entry = trust::match_entry(kb, icon, signer)) {
      return entry;
    }
  }
  for (const Run& run : runs) {
    if (run.folded.ends_with(".exe") && matcher.matches(run.folded)) {
      if (const rules::KbEntry* entry = trust::match_entry(kb, from_utf8(*run.path), signer)) {
        return entry;
      }
    }
  }
  return nullptr;
}

constexpr std::array<std::string_view, 12> runtimes = {
    "visual c++",          "vcredist",        ".net runtime",     "desktop runtime",
    "asp.net core",        "microsoft .net",  "directx",          "webview2",
    "windows app runtime", "windows app sdk", "microsoft.vclibs", "microsoft.net.native"};

bool runtime_name(std::string_view name) {
  return std::ranges::any_of(runtimes, [&](std::string_view part) { return contains(name, part); });
}

bool driver_name(std::string_view name) {
  const std::string folded = lower(name);
  return folded.find(" driver") != std::string::npos || folded.starts_with("driver") ||
         folded.find("chipset") != std::string::npos;
}

std::string first_word(std::string_view text) {
  const std::string folded = lower(trim(text));
  std::size_t end = 0;
  while (end < folded.size() && std::isalnum(static_cast<unsigned char>(folded[end])) != 0) {
    ++end;
  }
  return folded.substr(0, end);
}

// UTF-16LE text at `bytes` (up to a NUL) as UTF-8.
std::string utf8_of(std::span<const std::uint8_t> bytes) {
  std::string text;
  for (std::size_t at = 0; at + 1 < bytes.size(); at += 2) {
    std::uint32_t unit = bytes[at] | (static_cast<std::uint32_t>(bytes[at + 1]) << 8U);
    if (unit == 0) {
      break;
    }
    if (unit >= 0xD800 && unit < 0xDC00 && at + 3 < bytes.size()) {
      const std::uint32_t low = bytes[at + 2] | (static_cast<std::uint32_t>(bytes[at + 3]) << 8U);
      if (low >= 0xDC00 && low < 0xE000) {
        unit = 0x10000 + ((unit - 0xD800) << 10U) + (low - 0xDC00);
        at += 2;
      }
    }
    if (unit < 0x80) {
      text.push_back(static_cast<char>(unit));
    } else if (unit < 0x800) {
      text.push_back(static_cast<char>(0xC0 | (unit >> 6U)));
      text.push_back(static_cast<char>(0x80 | (unit & 0x3FU)));
    } else if (unit < 0x10000) {
      text.push_back(static_cast<char>(0xE0 | (unit >> 12U)));
      text.push_back(static_cast<char>(0x80 | ((unit >> 6U) & 0x3FU)));
      text.push_back(static_cast<char>(0x80 | (unit & 0x3FU)));
    } else {
      text.push_back(static_cast<char>(0xF0 | (unit >> 18U)));
      text.push_back(static_cast<char>(0x80 | ((unit >> 12U) & 0x3FU)));
      text.push_back(static_cast<char>(0x80 | ((unit >> 6U) & 0x3FU)));
      text.push_back(static_cast<char>(0x80 | (unit & 0x3FU)));
    }
  }
  return text;
}

std::uint32_t u32(std::span<const std::uint8_t> data, std::size_t at) {
  return data[at] | (static_cast<std::uint32_t>(data[at + 1]) << 8U) |
         (static_cast<std::uint32_t>(data[at + 2]) << 16U) |
         (static_cast<std::uint32_t>(data[at + 3]) << 24U);
}

std::uint64_t u64(std::span<const std::uint8_t> data, std::size_t at) {
  return u32(data, at) | (static_cast<std::uint64_t>(u32(data, at + 4)) << 32U);
}

Json last_used_json(const LastUsed& used, const ProcessHistory& history, WallTime now) {
  const auto days_of = [&](WallTime time) {
    return std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::days>(now - time).count());
  };
  if (used.time) {
    return Json{{"time", store::to_unix_seconds(*used.time)},
                {"days_ago", days_of(*used.time)},
                {"source", source_text(*used.source)},
                {"confidence", to_string(used.confidence)},
                {"text", date_text(*used.time) + " (" + source_text(*used.source) + ")"}};
  }
  const std::int64_t span = history.since() ? days_of(*history.since()) : 0;
  return Json{{"time", nullptr},
              {"days_ago", nullptr},
              {"source", nullptr},
              {"confidence", to_string(used.confidence)},
              {"text", "no usage recorded (MyClean's process history covers " +
                           std::to_string(span) + " day" + (span == 1 ? "" : "s") + ")"}};
}

}  // namespace

std::string_view to_string(Kind kind) {
  return kind == Kind::appx ? "appx" : "win32";
}

std::string_view to_string(UsageSource source) {
  switch (source) {
    case UsageSource::user_assist:
      return "UserAssist";
    case UsageSource::prefetch:
      return "Prefetch";
    case UsageSource::bam:
      return "BAM";
    case UsageSource::myclean:
      return "MyClean process history";
  }
  return "unknown";
}

std::string_view to_string(Confidence confidence) {
  switch (confidence) {
    case Confidence::low:
      return "low";
    case Confidence::medium:
      return "medium";
    case Confidence::high:
      return "high";
  }
  return "low";
}

std::string_view to_string(Verdict verdict) {
  switch (verdict) {
    case Verdict::removable:
      return "removable";
    case Verdict::keep:
      return "keep";
    case Verdict::unknown:
      return "unknown";
  }
  return "unknown";
}

Result<std::vector<Win32App>> win32_apps(const placement::IRegistry& registry) {
  std::vector<Win32App> apps;
  for (const std::string_view root : scanall::uninstall_roots) {
    Result<std::vector<std::string>> subkeys = registry.subkeys(root);
    if (!subkeys) {
      return tl::unexpected(subkeys.error());
    }
    for (const std::string& subkey : *subkeys) {
      const std::string key = std::string(root) + "\\" + subkey;
      Win32App app{.key = key, .name = read_text(registry, key, "DisplayName")};
      if (app.name.empty() || read_number(registry, key, "SystemComponent") == 1 ||
          !read_text(registry, key, "ParentKeyName").empty()) {
        continue;
      }
      app.publisher = trim(read_text(registry, key, "Publisher"));
      app.version = read_text(registry, key, "DisplayVersion");
      app.install_date = install_date(read_text(registry, key, "InstallDate"));
      app.location = from_utf8(trim(read_text(registry, key, "InstallLocation")));
      app.icon = from_utf8(program_of(read_text(registry, key, "DisplayIcon")));
      app.uninstall = read_text(registry, key, "UninstallString");
      if (trim(app.uninstall).empty()) {
        app.uninstall = read_text(registry, key, "QuietUninstallString");
      }
      app.estimated = Bytes{read_number(registry, key, "EstimatedSize") * kibibyte};
      apps.push_back(std::move(app));
    }
  }
  return apps;
}

Result<void> ProcessHistory::update(store::Store& store) {
  return store.read([&](store::Database& db) -> Result<void> {
    if (!since_) {
      Result<store::Statement> first = db.prepare("SELECT MIN(time) FROM proc_start");
      if (!first) {
        return tl::unexpected(first.error());
      }
      if (Result<bool> row = first->step(); row && *row && !first->is_null(0)) {
        since_ = store::from_unix_seconds(first->column_int(0));
      }
    }
    Result<store::Statement> query =
        db.prepare("SELECT image, MAX(time) FROM proc_start WHERE time > ?1 GROUP BY image");
    if (!query) {
      return tl::unexpected(query.error());
    }
    query->bind(1, newest_);
    Result<bool> row = query->step();
    for (; row && *row; row = query->step()) {
      const std::int64_t time = query->column_int(1);
      add(query->column_text(0), store::from_unix_seconds(time));
      newest_ = std::max(newest_, time);
    }
    return row ? Result<void>{} : tl::unexpected(row.error());
  });
}

void ProcessHistory::add(std::string_view image, WallTime time) {
  const std::string key = lower(image);
  const auto [found, inserted] = last_.try_emplace(key, time);
  if (!inserted) {
    found->second = std::max(found->second, time);
  }
  since_ = std::min(since_.value_or(time), time);
}

std::vector<fs::path> roots_of(const Win32App& app) {
  std::vector<fs::path> roots;
  const auto add = [&](const fs::path& folder) {
    const fs::path clean = folder.lexically_normal();
    if (!foreign_folder(clean) && clean.is_absolute() &&
        std::ranges::find(roots, clean) == roots.end()) {
      roots.push_back(clean);
    }
  };
  if (!app.location.empty()) {
    add(app.location);
  }
  if (roots.empty() && !app.icon.empty()) {
    add(app.icon.parent_path());
  }
  if (roots.empty() && !app.uninstall.empty() && !contains(app.uninstall, "msiexec")) {
    add(from_utf8(program_of(app.uninstall)).parent_path());
  }
  return roots;
}

LastUsed last_used(const App& app, std::span<const RunRecord> records,
                   const ProcessHistory& history, WallTime now) {
  return newest_run(Matcher(app), runs_of(records, history), history, now);
}

bool same_maker(std::string_view publisher, std::string_view manufacturer) {
  constexpr std::size_t min_letters = 3;
  if (contains(manufacturer, "system manufacturer") || contains(manufacturer, "o.e.m") ||
      contains(manufacturer, "default string")) {
    return false;
  }
  const std::string maker = first_word(manufacturer);
  const std::string word = first_word(publisher);
  if (maker.size() < min_letters || word.size() < min_letters) {
    return false;
  }
  return maker.starts_with(word) || word.starts_with(maker);
}

Classification classify(const App& app, bool system_appx, std::string_view manufacturer,
                        const rules::KbEntry* entry) {
  if (system_appx) {
    return {.verdict = Verdict::keep, .reason = "Windows system app"};
  }
  if (runtime_name(app.name) || (entry != nullptr && entry->category == "runtime")) {
    return {.verdict = Verdict::keep, .reason = "runtime or framework"};
  }
  if (driver_name(app.name) || (entry != nullptr && entry->category == "driver")) {
    return {.verdict = Verdict::keep, .reason = "driver"};
  }
  if (!manufacturer.empty() && same_maker(app.publisher, manufacturer)) {
    return {.verdict = Verdict::keep, .reason = "came with the computer (OEM)"};
  }
  if (entry != nullptr && entry->safe_to_remove) {
    return {.verdict = Verdict::removable,
            .reason = "the knowledge base marks it safe to remove (" + entry->id + ")"};
  }
  return {.verdict = Verdict::unknown,
          .reason = entry == nullptr ? "not in MyClean's knowledge base"
                                     : "the knowledge base does not mark it safe to remove"};
}

Facts read_facts(const placement::IRegistry& registry, const IAppSystem& system) {
  Facts facts;
  if (Result<std::vector<Win32App>> win32 = win32_apps(registry); win32) {
    facts.win32 = *std::move(win32);
  } else {
    facts.problems.push_back("Uninstall keys: " + win32.error().message);
  }
  if (Result<std::vector<AppxPackage>> appx = system.appx(); appx) {
    facts.appx = *std::move(appx);
  } else {
    facts.problems.push_back("Appx packages: " + appx.error().message);
  }
  const std::array<std::pair<std::string_view, Result<std::vector<RunRecord>>>, 3> sources = {
      std::pair{std::string_view("UserAssist"), system.user_assist()},
      std::pair{std::string_view("Prefetch"), system.prefetch()},
      std::pair{std::string_view("BAM"), system.bam()}};
  for (const auto& [name, records] : sources) {
    if (records) {
      facts.records.insert(facts.records.end(), records->begin(), records->end());
    } else {
      facts.problems.push_back(std::string(name) + ": " + records.error().message);
    }
  }
  if (Result<std::string> maker = system.manufacturer(); maker) {
    facts.manufacturer = *std::move(maker);
  } else {
    facts.problems.push_back("manufacturer: " + maker.error().message);
  }
  return facts;
}

std::vector<App> inventory(const Facts& facts, const ProcessHistory& history,
                           std::span<const rules::KbEntry> kb, WallTime now) {
  std::vector<App> apps;
  std::vector<bool> system;
  for (const Win32App& win32 : facts.win32) {
    App app{.kind = Kind::win32,
            .key = win32.key,
            .name = win32.name,
            .publisher = win32.publisher,
            .version = win32.version,
            .install_date = win32.install_date,
            .size = win32.estimated.count() > 0 ? std::optional(win32.estimated) : std::nullopt,
            .roots = roots_of(win32),
            .uninstall = trim(win32.uninstall)};
    apps.push_back(std::move(app));
    system.push_back(false);
  }
  for (const AppxPackage& package : facts.appx) {
    App app{.kind = Kind::appx,
            .key = package.full_name,
            .name = package.display_name.empty() ? package.full_name : package.display_name,
            .publisher = package.publisher,
            .version = package.version,
            .install_date = package.installed ? date_text(*package.installed) : std::string{},
            .size = package.size,
            .family = package.family_name};
    if (!package.location.empty()) {
      app.roots.push_back(package.location);
    }
    apps.push_back(std::move(app));
    system.push_back(package.system);
  }
  const std::vector<Run> runs = runs_of(facts.records, history);
  for (std::size_t index = 0; index < apps.size(); ++index) {
    App& app = apps[index];
    const Matcher matcher(app);
    app.last_used = newest_run(matcher, runs, history, now);
    const fs::path icon = index < facts.win32.size() ? facts.win32[index].icon : fs::path{};
    const rules::KbEntry* entry = kb_entry(app, matcher, icon, runs, kb);
    app.description = entry != nullptr ? entry->description : "unknown";
    app.category = entry != nullptr ? entry->category : std::string{};
    const Classification verdict = classify(app, system[index], facts.manufacturer, entry);
    app.verdict = verdict.verdict;
    app.reason = verdict.reason;
  }
  std::ranges::stable_sort(
      apps, {}, [](const App& app) { return std::pair{app.kind == Kind::appx, lower(app.name)}; });
  return apps;
}

bool unused(const App& app, std::chrono::days days, WallTime now) {
  return !app.last_used.time || now - *app.last_used.time >= days;
}

Json apps_json(std::span<const App> apps, const Facts& facts, const ProcessHistory& history,
               std::optional<int> unused_days, WallTime now) {
  Json list = Json::array();
  for (std::size_t index = 0; index < apps.size(); ++index) {
    const App& app = apps[index];
    if (unused_days && !unused(app, std::chrono::days{*unused_days}, now)) {
      continue;
    }
    list.push_back(
        Json{{"id", index + 1},
             {"kind", to_string(app.kind)},
             {"name", app.name},
             {"publisher", app.publisher.empty() ? Json(nullptr) : Json(app.publisher)},
             {"version", app.version.empty() ? Json(nullptr) : Json(app.version)},
             {"install_date", app.install_date.empty() ? Json(nullptr) : Json(app.install_date)},
             {"size", app.size ? Json(app.size->count()) : Json(nullptr)},
             {"description", app.description},
             {"category", app.category.empty() ? Json(nullptr) : Json(app.category)},
             {"last_used", last_used_json(app.last_used, history, now)},
             {"verdict", to_string(app.verdict)},
             {"reason", app.reason},
             {"uninstaller", app.kind == Kind::appx || !app.uninstall.empty()}});
  }
  const std::int64_t span =
      history.since()
          ? std::chrono::duration_cast<std::chrono::days>(now - *history.since()).count()
          : 0;
  return Json{
      {"apps", std::move(list)},
      {"count", apps.size()},
      {"unused_days", unused_days ? Json(*unused_days) : Json(nullptr)},
      {"history",
       {{"since", history.since() ? Json(store::to_unix_seconds(*history.since())) : Json(nullptr)},
        {"days", span}}},
      {"manufacturer", facts.manufacturer.empty() ? Json(nullptr) : Json(facts.manufacturer)},
      {"problems", facts.problems}};
}

std::string rot13(std::string_view text) {
  std::string result(text);
  for (char& c : result) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>('a' + ((c - 'a' + 13) % 26));
    } else if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>('A' + ((c - 'A' + 13) % 26));
    }
  }
  return result;
}

std::optional<WallTime> from_filetime(std::uint64_t ticks) {
  // FILETIME counts 100 ns since 1601-01-01; the Unix epoch is 11644473600 s later.
  constexpr std::uint64_t epoch_difference = 116'444'736'000'000'000;
  if (ticks <= epoch_difference) {
    return std::nullopt;
  }
  return WallTime{std::chrono::duration_cast<WallTime::duration>(
      std::chrono::nanoseconds{static_cast<std::int64_t>((ticks - epoch_difference) * 100)})};
}

std::optional<WallTime> user_assist_time(std::span<const std::uint8_t> data) {
  constexpr std::size_t record_size = 72;
  constexpr std::size_t count_at = 4;
  constexpr std::size_t time_at = 60;
  if (data.size() < record_size || u32(data, count_at) == 0) {
    return std::nullopt;
  }
  return from_filetime(u64(data, time_at));
}

namespace {

// The NUL-terminated UTF-16LE strings of `block`, as UTF-8.
std::vector<std::string> utf16_strings(std::span<const std::uint8_t> block) {
  std::vector<std::string> strings;
  std::size_t start = 0;
  for (std::size_t at = 0; at + 1 < block.size(); at += 2) {
    if (block[at] == 0 && block[at + 1] == 0) {
      if (at > start) {
        strings.push_back(utf8_of(block.subspan(start, at - start)));
      }
      start = at + 2;
    }
  }
  return strings;
}

}  // namespace

Result<PrefetchFile> parse_prefetch(std::span<const std::uint8_t> data) {
  // File header, then the file information of format 30/31 (Windows 10 and 11).
  constexpr std::size_t name_at = 0x10;
  constexpr std::size_t name_bytes = 60;
  constexpr std::size_t strings_offset_at = 0x64;
  constexpr std::size_t strings_size_at = 0x68;
  constexpr std::size_t volumes_offset_at = 0x6C;
  constexpr std::size_t volume_count_at = 0x70;
  constexpr std::size_t runs_at = 0x80;
  constexpr std::size_t run_slots = 8;
  constexpr std::size_t volume_entry = 96;
  constexpr std::size_t minimum = 0xD4;
  constexpr std::uint32_t format_30 = 30;
  constexpr std::uint32_t format_31 = 31;
  if (data.size() < minimum || data[4] != 'S' || data[5] != 'C' || data[6] != 'C' ||
      data[7] != 'A') {
    return fail(Errc::invalid_data, "not a decompressed Prefetch file");
  }
  const std::uint32_t format = u32(data, 0);
  if (format != format_30 && format != format_31) {
    return fail(Errc::invalid_data, "Prefetch format " + std::to_string(format) + " is not read");
  }
  PrefetchFile file{.executable = utf8_of(data.subspan(name_at, name_bytes))};
  for (std::size_t slot = 0; slot < run_slots; ++slot) {
    if (const std::optional<WallTime> run = from_filetime(u64(data, runs_at + (slot * 8)))) {
      file.runs.push_back(*run);
    }
  }
  std::ranges::sort(file.runs, std::ranges::greater{});
  const std::size_t strings = u32(data, strings_offset_at);
  const std::size_t strings_size = u32(data, strings_size_at);
  if (strings <= data.size() && strings_size <= data.size() - strings) {
    file.files = utf16_strings(data.subspan(strings, strings_size));
  }
  const std::size_t volumes = u32(data, volumes_offset_at);
  const std::size_t count = u32(data, volume_count_at);
  for (std::size_t index = 0; index < count; ++index) {
    const std::size_t entry = volumes + (index * volume_entry);
    if (entry > data.size() || data.size() - entry < volume_entry) {
      break;
    }
    const std::size_t path_at = volumes + u32(data, entry);
    const std::size_t path_bytes = static_cast<std::size_t>(u32(data, entry + 4)) * 2;
    if (path_at > data.size() || path_bytes > data.size() - path_at) {
      continue;
    }
    file.volumes.emplace_back(utf8_of(data.subspan(path_at, path_bytes)), u32(data, entry + 0x10));
  }
  return file;
}

std::string prefetch_executable(const PrefetchFile& file,
                                const std::map<std::uint32_t, std::string>& drive_of_serial) {
  const std::string suffix = "\\" + lower(file.executable);
  for (const std::string& name : file.files) {
    if (!lower(name).ends_with(suffix)) {
      continue;
    }
    for (const auto& [prefix, serial] : file.volumes) {
      if (!lower(name).starts_with(lower(prefix))) {
        continue;
      }
      const auto drive = drive_of_serial.find(serial);
      if (drive != drive_of_serial.end()) {
        return drive->second + name.substr(prefix.size());
      }
    }
  }
  return {};
}

}  // namespace myclean::apps
