#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/units.hpp"

// Browser extensions with their install origin (security spec, "Browser Extension Inventory
// With Install Origin"): every Chromium profile of Chrome, Edge and Brave (the extensions.settings
// of Preferences and Secure Preferences) and every Firefox profile (extensions.json), plus the
// extension ids each browser's policy forces (ExtensionInstallForcelist).
namespace myclean::security {

enum class Browser { chrome, edge, brave, firefox };

[[nodiscard]] std::string_view to_string(Browser browser);

// How the extension came to the profile.
enum class Origin {
  store,     // "store": the browser's web store (or addons.mozilla.org)
  external,  // "external": put there by a program, a registry entry or an external file
  policy,    // "policy": forced by a policy
  unpacked,  // "unpacked": loaded from a folder (developer mode) or a temporary add-on
};

[[nodiscard]] std::string_view to_string(Origin origin);

struct ChromiumProfile {
  Browser browser = Browser::chrome;
  std::string profile;                            // the folder name: "Default", "Profile 3"
  std::optional<std::string> preferences;         // the JSON text of Preferences
  std::optional<std::string> secure_preferences;  // the JSON text of Secure Preferences
};

struct FirefoxProfile {
  std::string profile;          // the folder name: "8rypaocz.default-release"
  std::string extensions_json;  // the JSON text of extensions.json
};

struct BrowserSnapshot {
  std::vector<ChromiumProfile> chromium;
  std::vector<FirefoxProfile> firefox;
  // The extension ids the policy of each browser forces (HKLM and the owner's HKCU).
  std::vector<std::pair<Browser, std::string>> forced;
  // A profile or policy that could not be read, with why; the others still are.
  std::vector<std::string> problems;
};

// The owner's browser profiles and the extension policies. Implemented by
// src/platform/security; tests use fakes. Read-only.
class IBrowserProfiles {
 public:
  virtual ~IBrowserProfiles() = default;
  IBrowserProfiles(const IBrowserProfiles&) = delete;
  IBrowserProfiles& operator=(const IBrowserProfiles&) = delete;
  IBrowserProfiles(IBrowserProfiles&&) = delete;
  IBrowserProfiles& operator=(IBrowserProfiles&&) = delete;

  [[nodiscard]] virtual BrowserSnapshot read() const = 0;

 protected:
  IBrowserProfiles() = default;
};

struct Extension {
  Browser browser = Browser::chrome;
  std::string profile;
  std::string id;
  std::string name;
  std::string version;
  std::optional<WallTime> installed;
  Origin origin = Origin::store;
  bool enabled = true;

  // Forced by a policy or put there from outside the browser: "probably not installed by you".
  [[nodiscard]] bool probably_not_by_owner() const {
    return origin == Origin::policy || origin == Origin::external;
  }
};

// The id of an ExtensionInstallForcelist value: "id;update_url" or just "id".
[[nodiscard]] std::string forced_id(std::string_view value);

// Unix time of a Chromium time: microseconds since 1601-01-01 as decimal text; nullopt when it
// is not one.
[[nodiscard]] std::optional<WallTime> chromium_time(std::string_view text);

// Every extension of `snapshot`, by browser, profile and name:
//  - Chromium: entries of extensions.settings with a location and a manifest (Secure Preferences
//    over Preferences for the same id); the browser's own component extensions (locations 5 and
//    10) are left out. Origin from the location: 1 store when from_webstore, else external (a
//    .crx installed from a file); 2, 3, 6 external; 4, 8 unpacked; 7, 9 policy. An id the
//    browser's policy forces is policy whatever its location. Disabled when disable_reasons is
//    non-zero or non-empty, or the older "state" is 0.
//  - Firefox: add-ons of type "extension", except the built-in and system ones (locations
//    app-builtin, app-system-defaults, app-system-addons). Origin store for addons.mozilla.org
//    installs, policy for enterprise-policy installs, unpacked for temporary add-ons, external
//    for foreign installs and everything else.
// A profile whose JSON does not parse is reported in `problems` and skipped.
[[nodiscard]] std::vector<Extension> extensions(const BrowserSnapshot& snapshot,
                                                std::vector<std::string>& problems);

}  // namespace myclean::security
