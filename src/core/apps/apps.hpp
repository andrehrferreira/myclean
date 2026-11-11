#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/settings.hpp"
#include "core/rules/schema.hpp"
#include "core/store/store.hpp"

// Installed apps (apps spec): every app of the Uninstall keys and the owner's Appx packages, with
// size, publisher, the knowledge-base description, when it was last used (fused from UserAssist,
// Prefetch, BAM and MyClean's own process-start history, each answer with its source and
// confidence) and whether it can be removed. Read-only; uninstalling is a plan the owner approves
// (cleaning::Executor::propose_uninstall). The OS facts come from IAppSystem (src/platform/apps).
namespace myclean::apps {

enum class Kind { win32, appx };
[[nodiscard]] std::string_view to_string(Kind kind);

// Where a run record comes from.
enum class UsageSource {
  user_assist,  // "UserAssist": programs the owner started from Explorer or Start
  prefetch,     // "Prefetch": C:\Windows\Prefetch\*.pf, the last runs Windows recorded
  bam,          // "BAM": the Background Activity Moderator's last run per program
  myclean,      // "MyClean process history": table proc_start
};
[[nodiscard]] std::string_view to_string(UsageSource source);

enum class Confidence { low, medium, high };
[[nodiscard]] std::string_view to_string(Confidence confidence);

enum class Verdict { removable, keep, unknown };
[[nodiscard]] std::string_view to_string(Verdict verdict);

// One Appx package of the owner (PackageManager.FindPackagesForUser), main packages only.
struct AppxPackage {
  std::string full_name;    // "Microsoft.WindowsCalculator_11.2405.2.0_x64__8wekyb3d8bbwe"
  std::string family_name;  // "Microsoft.WindowsCalculator_8wekyb3d8bbwe"
  std::string display_name;
  std::string publisher;  // PublisherDisplayName
  std::string version;
  std::filesystem::path location;  // InstalledLocation
  std::optional<WallTime> installed;
  std::optional<Bytes> size;  // the files of its folder; nullopt when not measured
  bool system = false;        // SignatureKind::System
};

// One run of a program as a source recorded it.
struct RunRecord {
  UsageSource source = UsageSource::user_assist;
  // A full executable path ("C:\Program Files\App\app.exe"), or an Appx app id
  // ("<family name>!<app>") or family name.
  std::string path;
  WallTime last_run;
};

// What Windows holds about the installed apps and their use, as the OS stores it. Implemented by
// src/platform/apps for the owner; tests use fakes. Each read fails on its own.
class IAppSystem {
 public:
  virtual ~IAppSystem() = default;
  IAppSystem(const IAppSystem&) = delete;
  IAppSystem& operator=(const IAppSystem&) = delete;
  IAppSystem(IAppSystem&&) = delete;
  IAppSystem& operator=(IAppSystem&&) = delete;

  [[nodiscard]] virtual Result<std::vector<AppxPackage>> appx() const = 0;
  [[nodiscard]] virtual Result<std::vector<RunRecord>> user_assist() const = 0;
  [[nodiscard]] virtual Result<std::vector<RunRecord>> prefetch() const = 0;
  [[nodiscard]] virtual Result<std::vector<RunRecord>> bam() const = 0;
  // Win32_ComputerSystem.Manufacturer ("ASUS", "Dell Inc.").
  [[nodiscard]] virtual Result<std::string> manufacturer() const = 0;

 protected:
  IAppSystem() = default;
};

// One Win32 app of the Uninstall keys.
struct Win32App {
  std::string key;  // "<one of scanall::uninstall_roots>\<subkey>"
  std::string name;
  std::string publisher;
  std::string version;
  std::string install_date;        // InstallDate as "YYYY-MM-DD"; empty when absent or not a date
  std::filesystem::path location;  // InstallLocation
  std::filesystem::path icon;      // DisplayIcon without its ",<index>"
  std::string uninstall;           // UninstallString, else QuietUninstallString; empty: none
  Bytes estimated;                 // EstimatedSize (KiB) in bytes; zero when absent
};

// Every entry of the Uninstall keys a person sees in Settings > Apps: with a DisplayName, not a
// SystemComponent and not an update of another entry (ParentKeyName).
[[nodiscard]] Result<std::vector<Win32App>> win32_apps(const placement::IRegistry& registry);

// MyClean's own process starts (table proc_start), per lower-case image path: the newest start,
// and since when the history reaches back. update() reads only the rows newer than what it holds.
class ProcessHistory {
 public:
  [[nodiscard]] Result<void> update(store::Store& store);

  [[nodiscard]] const std::map<std::string, WallTime, std::less<>>& last() const { return last_; }
  [[nodiscard]] std::optional<WallTime> since() const { return since_; }

  // For tests and callers that hold the starts already.
  void add(std::string_view image, WallTime time);

 private:
  std::map<std::string, WallTime, std::less<>> last_;
  std::optional<WallTime> since_;
  std::int64_t newest_ = 0;  // Unix seconds of the newest row read
};

// The history spans at least this long before own history alone is trusted (apps spec,
// "Unused App Filter").
inline constexpr std::chrono::days authoritative_history{180};

struct LastUsed {
  std::optional<WallTime> time;  // nullopt: no usage recorded
  std::optional<UsageSource> source;
  Confidence confidence = Confidence::low;
};

// One installed app with its facts and verdict.
struct App {
  Kind kind = Kind::win32;
  std::string key;  // the Uninstall key, or the package full name
  std::string name;
  std::string publisher;
  std::string version;
  std::string install_date;  // "YYYY-MM-DD"; empty when unknown
  std::optional<Bytes> size;
  std::string description;                   // the knowledge base's, or "unknown"
  std::string category;                      // the knowledge base's; empty when unknown
  std::vector<std::filesystem::path> roots;  // the folders its programs run from
  std::string family;                        // appx: the package family name
  LastUsed last_used;
  Verdict verdict = Verdict::unknown;
  std::string reason;
  std::string uninstall;  // win32: what runs to uninstall it; empty: no uninstaller registered
};

// The folders a Win32 app's programs run from: InstallLocation, else the folder of DisplayIcon,
// else the folder of the UninstallString's program; never a Windows, Installer cache or Package
// Cache folder (the uninstaller lives there, not the app).
[[nodiscard]] std::vector<std::filesystem::path> roots_of(const Win32App& app);

// The newest run of `app` among `records` (a path inside one of its roots, or its Appx app id or
// family name) and `history`, with its source; confidence high for Prefetch, BAM and MyClean's
// history once it spans authoritative_history, medium for UserAssist (Explorer starts only) and
// a shorter own history. No run: no time, confidence high when the own history spans
// authoritative_history, else low.
[[nodiscard]] LastUsed last_used(const App& app, std::span<const RunRecord> records,
                                 const ProcessHistory& history, WallTime now);

// The verdict of `app` (apps spec, "Keep Or Remove Verdict"): keep for a system Appx package
// ("Windows system app"), a runtime or framework (Visual C++ redistributables, .NET, DirectX,
// WebView2, the Windows App Runtime, or a knowledge-base entry of category runtime: "runtime or
// framework"), a driver (a name naming one, or category driver: "driver"), an app of the
// computer's maker `manufacturer` ("came with the computer (OEM)"); removable only when its
// knowledge-base entry marks it safe to remove; unknown otherwise. `entry` is its knowledge-base
// entry (may be null).
struct Classification {
  Verdict verdict = Verdict::unknown;
  std::string reason;
};
[[nodiscard]] Classification classify(const App& app, bool system_appx,
                                      std::string_view manufacturer, const rules::KbEntry* entry);

// Whether `publisher` is the maker `manufacturer` ("ASUSTeK Computer Inc." for "ASUS"): the
// first words, lower-case, one a prefix of the other, three letters at least. Placeholder makers
// ("System manufacturer", "To Be Filled By O.E.M.") match nothing.
[[nodiscard]] bool same_maker(std::string_view publisher, std::string_view manufacturer);

// What inventory() reads.
struct Facts {
  std::vector<Win32App> win32;
  std::vector<AppxPackage> appx;
  std::vector<RunRecord> records;  // every source's runs
  std::string manufacturer;
  std::vector<std::string> problems;  // sources that could not be read
};

// Reads every source; a source that fails is a problem and adds nothing.
[[nodiscard]] Facts read_facts(const placement::IRegistry& registry, const IAppSystem& system);

// The apps of `facts`, Win32 then Appx, each by name, with last_used, classify and the
// knowledge-base entry (`kb`) of the first of its programs that matches one (DisplayIcon, then the
// programs its runs name).
[[nodiscard]] std::vector<App> inventory(const Facts& facts, const ProcessHistory& history,
                                         std::span<const rules::KbEntry> kb, WallTime now);

// Whether `app` counts as unused: not used in `days`, or no usage recorded.
[[nodiscard]] bool unused(const App& app, std::chrono::days days, WallTime now);

// The apps.list answer (core/ipc/apps.hpp) for `apps` with their ids (1-based positions in
// `apps`), keeping only the unused ones when `unused_days` is set.
[[nodiscard]] nlohmann::json apps_json(std::span<const App> apps, const Facts& facts,
                                       const ProcessHistory& history,
                                       std::optional<int> unused_days, WallTime now);

// ---- Pure parsers of the usage sources (used by src/platform/apps) ---------------------------

// ROT13 of the ASCII letters of `text` (UserAssist value names).
[[nodiscard]] std::string rot13(std::string_view text);

// A UserAssist Count value (Windows 7 and later, 72 bytes): the run count at offset 4 and the
// FILETIME of the last run at 60; nullopt when it is shorter or holds no run.
[[nodiscard]] std::optional<WallTime> user_assist_time(std::span<const std::uint8_t> data);

// A decompressed Prefetch file (format 30 and 31: Windows 10 and 11, "SCCA" at offset 4).
struct PrefetchFile {
  std::string executable;          // the executable name it records, upper case
  std::vector<WallTime> runs;      // the last run times, newest first (up to 8)
  std::vector<std::string> files;  // the file names it references (device paths, upper case)
  // The volumes it references: the device path prefix of `files` ("\VOLUME{01d...-1a2b3c4d}")
  // and the serial number of each.
  std::vector<std::pair<std::string, std::uint32_t>> volumes;
};
[[nodiscard]] Result<PrefetchFile> parse_prefetch(std::span<const std::uint8_t> data);

// The full path of the executable of `file`: the referenced file that ends with "\<executable>",
// its volume prefix replaced through `drive_of_serial` (volume serial -> "C:"); empty when none
// matches or the volume has no drive letter.
[[nodiscard]] std::string prefetch_executable(
    const PrefetchFile& file, const std::map<std::uint32_t, std::string>& drive_of_serial);

// FILETIME (100 ns since 1601) as a WallTime; nullopt for zero.
[[nodiscard]] std::optional<WallTime> from_filetime(std::uint64_t ticks);

}  // namespace myclean::apps
