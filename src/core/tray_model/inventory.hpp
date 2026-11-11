#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

// The tray's reading of the inventory methods for the Apps, Security and Backups screens
// (contracts in core/ipc/apps.hpp, security.hpp and backup.hpp). They only read; the one change
// they offer, an uninstall, is a plan that waits for the owner's approval in Cleanup, and a
// Defender scan of one item starts only on the owner's click and remediates nothing.
namespace myclean::tray_model {

// ---- Apps -------------------------------------------------------------------------------------

// One app of apps.list.
struct AppRow {
  std::int64_t id = 0;  // its number in that answer
  std::string kind;     // win32, appx
  std::string name;
  std::string publisher;
  std::string version;
  std::string install_date;
  std::optional<Bytes> size;
  std::string description;  // "unknown" when the knowledge base does not know it
  std::optional<WallTime> last_used;
  std::optional<std::int64_t> days_ago;
  std::string last_used_source;  // UserAssist, Prefetch, BAM, MyClean process history; or empty
  std::string confidence;
  std::string last_used_text;  // what the service says about it, also when no use is recorded
  std::string verdict;         // removable, keep, unknown
  std::string reason;
  bool uninstaller = false;
};
struct AppsList {
  std::vector<AppRow> apps;
  std::int64_t count = 0;
  std::int64_t history_days = 0;  // how far MyClean's process history reaches
  std::string manufacturer;
  std::vector<std::string> problems;
};
[[nodiscard]] Result<AppsList> parse_apps_list(const nlohmann::json& reply);

// What names an app across apps.list answers: its kind, name, version and publisher. A proposal
// sends the id the app has in a fresh answer.
[[nodiscard]] std::string app_key(const AppRow& app);
[[nodiscard]] std::string app_key(const nlohmann::json& app);
// Not used in `days` days, or no use recorded at all.
[[nodiscard]] bool unused_for(const AppRow& app, std::int64_t days);

// ---- Security ---------------------------------------------------------------------------------

// One persistence point of security.scan.
struct PersistenceRow {
  std::int64_t id = 0;  // its number in that answer
  std::string source;
  std::string location;
  std::string name;
  std::string command;
  std::string image;  // the file it runs, on disk; empty when it does not exist
  std::optional<bool> enabled;
  std::string signer;
  std::string startup_item;  // the startup.list key when it is a startup item
  std::string status;        // default, ok, suspicious, known_dangerous
  std::vector<std::string> reasons;
  std::string summary;
  std::string verdict_source;  // Microsoft Defender, VirusTotal; empty without a verdict
  std::string threat;
  std::string virustotal;  // "VirusTotal: 0 of 72 engines flag it"; empty without a lookup
};
struct DefenderDetectionRow {
  std::string threat;
  std::vector<std::string> resources;
  std::optional<WallTime> time;
};
struct SecurityScan {
  std::vector<PersistenceRow> items;
  std::int64_t defaults = 0;
  std::int64_t ok = 0;
  std::int64_t suspicious = 0;
  std::int64_t known_dangerous = 0;
  bool defender_available = false;
  std::string defender_error;
  std::vector<DefenderDetectionRow> detections;
  bool virustotal_enabled = false;
  std::string virustotal_note;
  std::int64_t virustotal_queued = 0;
  std::vector<std::string> problems;
};
[[nodiscard]] Result<SecurityScan> parse_security_scan(const nlohmann::json& reply);

// What names a persistence point across security.scan answers: its source, location and name.
[[nodiscard]] std::string persistence_key(const PersistenceRow& item);
[[nodiscard]] std::string persistence_key(const nlohmann::json& item);
// The label of `item`: "known dangerous (Microsoft Defender: <threat>)" only when a verdict says
// so, "suspicious", "ok" or "Windows default". Without a verdict nothing is called dangerous.
[[nodiscard]] std::string trust_label(const PersistenceRow& item);
// Whether `item` is labelled known dangerous: by a verdict only.
[[nodiscard]] bool known_dangerous(const PersistenceRow& item);

// One browser extension of security.extensions.
struct ExtensionRow {
  std::string browser;
  std::string profile;
  std::string id;
  std::string name;
  std::string version;
  std::string origin;  // store, external, policy, unpacked
  bool enabled = false;
  std::string flag;  // "probably not installed by you", or empty
};
struct Extensions {
  std::vector<ExtensionRow> extensions;
  std::vector<std::string> problems;
};
[[nodiscard]] Result<Extensions> parse_security_extensions(const nlohmann::json& reply);

// One process of security.network.
struct RemoteRow {
  std::string address;
  std::string name;  // reverse DNS; empty while off or unresolved
  std::int64_t port = 0;
  std::string protocol;
  bool open_now = false;
};
struct TalkerRow {
  std::uint32_t pid = 0;
  std::string image;
  bool running = false;
  std::optional<bool> visible_window;
  std::string signer;
  bool unrecognized = false;
  std::vector<std::string> reasons;
  Bytes sent;
  Bytes received;
  std::int64_t active_minutes = 0;
  std::vector<RemoteRow> remotes;
  std::vector<std::string> listening;  // "tcp 445"
};
struct NetworkTalkers {
  double hours = 0.0;
  bool reverse_dns = false;
  bool windows_reported = false;
  std::vector<TalkerRow> processes;
};
[[nodiscard]] Result<NetworkTalkers> parse_security_network(const nlohmann::json& reply);
// "no window", "a visible window", "unknown".
[[nodiscard]] std::string_view window_text(const std::optional<bool>& visible);

// ---- Backups ----------------------------------------------------------------------------------

struct BackupSourceRow {
  std::string source;
  std::string name;
  std::string status;  // found, not_found, could_not_check
  std::string location;
  std::optional<WallTime> newest;
  std::optional<std::int64_t> age_days;
  std::int64_t copies = 0;
  bool counts_toward_age = false;
  std::string detail;
  std::vector<std::string> extra;  // the products, the folders, or the artifacts found
};
struct UnprotectedRepoRow {
  std::string path;
  std::string text;
};
struct BackupCheck {
  std::string state;  // checking, ready, failed
  std::optional<WallTime> checked_at;
  std::string error;
  std::vector<BackupSourceRow> sources;
  bool has_last_backup = false;
  std::string last_name;
  std::string last_location;
  std::optional<WallTime> last_time;
  std::int64_t last_age_days = 0;
  bool backup_exists = false;
  std::string summary;
  std::string recommendation;  // empty when a backup exists
  std::vector<std::string> recommended_drives;
  std::vector<std::string> left_out;  // "K: (reason)"
  bool repositories_checked = false;
  std::vector<std::string> workspace_roots;  // where the check looks for repositories
  std::int64_t repositories = 0;
  std::vector<UnprotectedRepoRow> unprotected;
  std::vector<std::string> repository_problems;
};
[[nodiscard]] Result<BackupCheck> parse_backups_check(const nlohmann::json& reply);
// "found", "not found", "could not be checked".
[[nodiscard]] std::string_view backup_status_text(std::string_view status);

}  // namespace myclean::tray_model
