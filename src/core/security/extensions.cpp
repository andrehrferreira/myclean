#include "core/security/extensions.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/enum_names.hpp"
#include "core/model/units.hpp"

namespace myclean::security {
namespace {

using Json = nlohmann::json;

constexpr std::array<std::string_view, 4> browser_names = {"chrome", "edge", "brave", "firefox"};
constexpr std::array<std::string_view, 4> origin_names = {"store", "external", "policy",
                                                          "unpacked"};

// Microseconds between 1601-01-01 and 1970-01-01.
constexpr std::int64_t chromium_epoch_offset = 11'644'473'600'000'000;

// Chromium's Manifest::Location values.
constexpr int location_internal = 1;
constexpr int location_external_pref = 2;
constexpr int location_external_registry = 3;
constexpr int location_unpacked = 4;
constexpr int location_component = 5;
constexpr int location_external_pref_download = 6;
constexpr int location_external_policy_download = 7;
constexpr int location_command_line = 8;
constexpr int location_external_policy = 9;
constexpr int location_external_component = 10;

std::string text(const Json& object, std::string_view key) {
  const auto found = object.find(key);
  return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

// The extensions.settings object of a Preferences text; null when it has none. Fails (nullopt)
// when the text is not JSON.
std::optional<Json> settings_of(const std::optional<std::string>& preferences) {
  if (!preferences) {
    return Json::object();
  }
  Json document = Json::parse(*preferences, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return std::nullopt;
  }
  const auto extensions = document.find("extensions");
  if (extensions == document.end() || !extensions->is_object()) {
    return Json::object();
  }
  const auto settings = extensions->find("settings");
  if (settings == extensions->end() || !settings->is_object()) {
    return Json::object();
  }
  return *settings;
}

bool disabled(const Json& entry) {
  if (const auto reasons = entry.find("disable_reasons"); reasons != entry.end()) {
    if (reasons->is_number_integer() && reasons->get<std::int64_t>() != 0) {
      return true;
    }
    if (reasons->is_array() && !reasons->empty()) {
      return true;
    }
  }
  const auto state = entry.find("state");
  return state != entry.end() && state->is_number_integer() && state->get<std::int64_t>() == 0;
}

std::optional<Origin> chromium_origin(int location, bool from_webstore) {
  switch (location) {
    case location_internal:
      return from_webstore ? Origin::store : Origin::external;
    case location_external_pref:
    case location_external_registry:
    case location_external_pref_download:
      return Origin::external;
    case location_unpacked:
    case location_command_line:
      return Origin::unpacked;
    case location_external_policy_download:
    case location_external_policy:
      return Origin::policy;
    case location_component:
    case location_external_component:
    default:
      return std::nullopt;
  }
}

void add_chromium(const ChromiumProfile& profile, const std::set<std::string>& forced,
                  std::vector<Extension>& found, std::vector<std::string>& problems) {
  const std::optional<Json> plain = settings_of(profile.preferences);
  const std::optional<Json> secure = settings_of(profile.secure_preferences);
  if (!plain || !secure) {
    problems.push_back(std::string(to_string(profile.browser)) + " profile " + profile.profile +
                       ": " + (plain ? "Secure Preferences" : "Preferences") + " is not JSON");
    if (!plain && !secure) {
      return;
    }
  }
  Json merged = plain.value_or(Json::object());
  const Json overrides = secure.value_or(Json::object());
  for (const auto& [id, entry] : overrides.items()) {
    if (!entry.is_object()) {
      continue;
    }
    auto& target = merged[id];
    if (!target.is_object()) {
      target = Json::object();
    }
    target.update(entry);
  }
  for (const auto& [id, entry] : merged.items()) {
    if (!entry.is_object()) {
      continue;
    }
    const auto location = entry.find("location");
    const auto manifest = entry.find("manifest");
    if (location == entry.end() || !location->is_number_integer() || manifest == entry.end() ||
        !manifest->is_object()) {
      continue;  // a stub (sync or ordering data) of an extension that is not installed
    }
    const auto from_webstore = entry.find("from_webstore");
    std::optional<Origin> origin = chromium_origin(
        location->get<int>(),
        from_webstore != entry.end() && from_webstore->is_boolean() && from_webstore->get<bool>());
    if (!origin) {
      continue;  // a component of the browser itself
    }
    if (forced.contains(id)) {
      origin = Origin::policy;
    }
    std::string installed = text(entry, "first_install_time");
    if (installed.empty()) {
      installed = text(entry, "install_time");
    }
    found.push_back(Extension{.browser = profile.browser,
                              .profile = profile.profile,
                              .id = id,
                              .name = text(*manifest, "name"),
                              .version = text(*manifest, "version"),
                              .installed = chromium_time(installed),
                              .origin = *origin,
                              .enabled = !disabled(entry)});
  }
}

bool builtin_firefox_location(std::string_view location) {
  return location == "app-builtin" || location == "app-system-defaults" ||
         location == "app-system-addons";
}

Origin firefox_origin(const Json& addon, const std::set<std::string>& forced,
                      const std::string& id) {
  const std::string location = text(addon, "location");
  std::string source;
  if (const auto info = addon.find("installTelemetryInfo");
      info != addon.end() && info->is_object()) {
    source = text(*info, "source");
  }
  if (forced.contains(id) || source == "enterprise-policy") {
    return Origin::policy;
  }
  if (location == "app-temporary" || source == "temporary-addon" || source == "about:debugging") {
    return Origin::unpacked;
  }
  const auto foreign = addon.find("foreignInstall");
  if (foreign != addon.end() && foreign->is_boolean() && foreign->get<bool>()) {
    return Origin::external;
  }
  if (location == "app-profile" &&
      (source == "amo" || source == "disco" || source == "about:addons" || source == "sync" ||
       text(addon, "sourceURI").starts_with("https://addons.mozilla.org/"))) {
    return Origin::store;
  }
  return Origin::external;
}

void add_firefox(const FirefoxProfile& profile, const std::set<std::string>& forced,
                 std::vector<Extension>& found, std::vector<std::string>& problems) {
  const Json document = Json::parse(profile.extensions_json, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    problems.push_back("firefox profile " + profile.profile + ": extensions.json is not JSON");
    return;
  }
  const auto addons = document.find("addons");
  if (addons == document.end() || !addons->is_array()) {
    return;
  }
  for (const Json& addon : *addons) {
    if (!addon.is_object() || text(addon, "type") != "extension" ||
        builtin_firefox_location(text(addon, "location"))) {
      continue;
    }
    const std::string id = text(addon, "id");
    std::string name;
    if (const auto locale = addon.find("defaultLocale");
        locale != addon.end() && locale->is_object()) {
      name = text(*locale, "name");
    }
    std::optional<WallTime> installed;
    if (const auto date = addon.find("installDate");
        date != addon.end() && date->is_number_integer()) {
      installed = WallTime{std::chrono::milliseconds{date->get<std::int64_t>()}};
    }
    const auto active = addon.find("active");
    found.push_back(Extension{
        .browser = Browser::firefox,
        .profile = profile.profile,
        .id = id,
        .name = name,
        .version = text(addon, "version"),
        .installed = installed,
        .origin = firefox_origin(addon, forced, id),
        .enabled = active == addon.end() || !active->is_boolean() || active->get<bool>()});
  }
}

std::set<std::string> forced_of(const BrowserSnapshot& snapshot, Browser browser) {
  std::set<std::string> ids;
  for (const auto& [forced_browser, id] : snapshot.forced) {
    if (forced_browser == browser) {
      ids.insert(id);
    }
  }
  return ids;
}
}  // namespace

std::string_view to_string(Browser browser) {
  return detail::enum_name(browser_names, browser);
}

std::string_view to_string(Origin origin) {
  return detail::enum_name(origin_names, origin);
}

std::string forced_id(std::string_view value) {
  const std::size_t separator = value.find(';');
  std::string_view id = value.substr(0, separator);
  while (!id.empty() && id.front() == ' ') {
    id.remove_prefix(1);
  }
  while (!id.empty() && id.back() == ' ') {
    id.remove_suffix(1);
  }
  return std::string(id);
}

std::optional<WallTime> chromium_time(std::string_view text) {
  std::int64_t microseconds = 0;
  const char* const first = std::to_address(text.begin());
  const char* const last = std::to_address(text.end());
  const auto [end, error] = std::from_chars(first, last, microseconds);
  if (text.empty() || error != std::errc{} || end != last || microseconds < chromium_epoch_offset) {
    return std::nullopt;
  }
  return WallTime{std::chrono::duration_cast<WallTime::duration>(
      std::chrono::microseconds{microseconds - chromium_epoch_offset})};
}

std::vector<Extension> extensions(const BrowserSnapshot& snapshot,
                                  std::vector<std::string>& problems) {
  std::vector<Extension> found;
  for (const ChromiumProfile& profile : snapshot.chromium) {
    add_chromium(profile, forced_of(snapshot, profile.browser), found, problems);
  }
  for (const FirefoxProfile& profile : snapshot.firefox) {
    add_firefox(profile, forced_of(snapshot, Browser::firefox), found, problems);
  }
  std::ranges::stable_sort(found, [](const Extension& a, const Extension& b) {
    return std::tie(a.browser, a.profile, a.name, a.id) <
           std::tie(b.browser, b.profile, b.name, b.id);
  });
  return found;
}

}  // namespace myclean::security
