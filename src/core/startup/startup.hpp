#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

// The startup inventory (startup spec, "Startup Inventory", "Disable and Enable Through Native
// Mechanisms"): every automatic start source of this machine, read through IStartupSystem, and
// the state each item is in. The byte layout and the sources are those of spike X6
// (docs/spikes/X6-startup.md).
namespace myclean::startup {

// Where an item starts from.
enum class Source {
  run,             // "run": a Run value, 64-bit view (HKCU or HKLM)
  run32,           // "run32": a Run value, HKLM\Software\WOW6432Node
  run_once,        // "runonce": a RunOnce value, 64-bit view; no enabled state
  run_once32,      // "runonce32": a RunOnce value under WOW6432Node; no enabled state
  startup_folder,  // "folder": a file in the user's or the common Startup folder
  task,            // "task": a scheduled task with a logon or boot trigger
  service,         // "service": an auto-start service not published by Microsoft Windows
};

[[nodiscard]] std::string_view to_string(Source source);
[[nodiscard]] std::optional<Source> parse_source(std::string_view text);

// Whose it is: HKCU and the user's Startup folder, or HKLM, the common Startup folder, tasks and
// services.
enum class Scope { user, machine };

[[nodiscard]] std::string_view to_string(Scope scope);
[[nodiscard]] std::optional<Scope> parse_scope(std::string_view text);

// The StartupApproved subkey that holds the state of a Run, Run32 or Startup-folder item.
enum class ApprovedKey { run, run32, startup_folder };

// A StartupApproved value as stored: REG_BINARY, normally 12 bytes (a flags DWORD, then the
// FILETIME of the disable). nullopt: no value.
using ApprovedValue = std::optional<std::vector<std::uint8_t>>;

// Byte 0 with bit 0 set (03, 07) is disabled; 02, any other value and no value are enabled.
[[nodiscard]] bool approved_disabled(const ApprovedValue& value);
// What Task Manager writes: 03 00 00 00 + the FILETIME of `when` (UTC) to disable,
// 02 00 00 00 + eight zero bytes to enable.
[[nodiscard]] std::vector<std::uint8_t> approved_bytes(bool disabled, WallTime when);

// How a service starts. Only `automatic` services are startup items.
enum class StartType { boot, system, automatic, manual, disabled };

[[nodiscard]] std::string_view to_string(StartType type);
[[nodiscard]] std::optional<StartType> parse_start_type(std::string_view text);

struct ServiceStart {
  StartType type = StartType::manual;
  bool delayed = false;  // Automatic (Delayed Start); kept apart from the type, as Windows does

  friend bool operator==(const ServiceStart&, const ServiceStart&) = default;
};

// What IStartupSystem reads, source by source, before any interpretation.
struct RunValue {
  Scope scope = Scope::user;
  bool wow64 = false;  // read from WOW6432Node
  bool once = false;   // RunOnce
  std::string name;    // byte-exact: trailing spaces and backslashes are part of it
  std::string command;
};

struct StartupFile {
  Scope scope = Scope::user;
  std::string file_name;  // "Discord.lnk", also files that are not shortcuts
  std::string target;     // a shortcut's target and arguments; the file's path otherwise
};

struct Approved {
  Scope scope = Scope::user;
  ApprovedKey key = ApprovedKey::run;
  std::string name;
  std::vector<std::uint8_t> bytes;
};

struct Task {
  std::string path;  // "\OneDrive Startup Task-S-1-5-21-..."
  bool enabled = true;
  bool logon = false;  // has a logon trigger
  bool boot = false;   // has a boot trigger
  std::string command;
  std::optional<std::string> signer;  // of the first exec action's image, when signed and valid
};

struct Service {
  std::string name;
  std::string display_name;
  ServiceStart start;
  std::string binary;  // ImagePath, or Parameters\ServiceDll for a shared svchost service
  std::optional<std::string> signer;  // when signed and valid
};

struct Snapshot {
  std::vector<RunValue> run;
  std::vector<StartupFile> files;
  std::vector<Approved> approved;
  std::vector<Task> tasks;
  std::vector<Service> services;
};

// One startup item, identified by `id`: "<source>:<scope>:<name>", the name byte-exact (a task's
// full path, a service's key name, a Startup-folder file name).
struct Item {
  std::string id;
  Source source = Source::run;
  Scope scope = Scope::user;
  std::string name;
  std::string display_name;  // the service's display name; `name` for the others
  std::string command;
  // nullopt for RunOnce, which StartupApproved does not cover.
  std::optional<bool> enabled;
  ApprovedValue approved;             // run, run32 and folder items: the value as stored
  std::optional<ServiceStart> start;  // services
  std::optional<std::string> signer;  // tasks and services
};

[[nodiscard]] std::string item_id(Source source, Scope scope, std::string_view name);

struct ItemKey {
  Source source = Source::run;
  Scope scope = Scope::user;
  std::string name;
};

// Fails with invalid_argument when `id` is not "<source>:<scope>:<name>".
[[nodiscard]] Result<ItemKey> parse_item_id(std::string_view id);

// The signer of every Windows component. "Microsoft Windows Hardware Compatibility Publisher"
// signs third-party drivers and services (WHQL) and "Microsoft Corporation" signs optional
// products: both are listed.
inline constexpr std::string_view windows_signer = "Microsoft Windows";

// The items of `snapshot`, in source order (run, run32, runonce, runonce32, folder, task,
// service), then by scope (user first) and name:
//  - a Run, Run32 or folder item takes its state from the StartupApproved value of its own hive
//    (a common Startup-folder file: HKLM's); StartupApproved values with no item are leftovers
//    and not listed;
//  - desktop.ini in a Startup folder is not an item;
//  - a task is listed when it has a logon or boot trigger, unless its image is signed by
//    Microsoft Windows or it has no image (a COM handler) and lives under \Microsoft\Windows\;
//  - a service is listed when its start type is automatic and its binary is not signed by
//    Microsoft Windows.
[[nodiscard]] std::vector<Item> inventory(const Snapshot& snapshot);

// The item with this id, if listed.
[[nodiscard]] const Item* find_item(std::span<const Item> items, std::string_view id);

// Whether an item can be disabled and enabled: every source but RunOnce.
[[nodiscard]] bool toggleable(const Item& item);

// The automatic start sources as the OS stores them. Implemented by src/platform/startup; tests
// use fakes. The service reads HKCU and the user's Startup folder of the owner it was given.
class IStartupSystem {
 public:
  virtual ~IStartupSystem() = default;
  IStartupSystem(const IStartupSystem&) = delete;
  IStartupSystem& operator=(const IStartupSystem&) = delete;
  IStartupSystem(IStartupSystem&&) = delete;
  IStartupSystem& operator=(IStartupSystem&&) = delete;

  [[nodiscard]] virtual Result<Snapshot> read() const = 0;
  // Writes (or, for nullopt, deletes) the StartupApproved value `name` under `key` of the hive
  // of `scope`. HKLM needs elevation.
  [[nodiscard]] virtual Result<void> set_approved(Scope scope, ApprovedKey key,
                                                  std::string_view name,
                                                  const ApprovedValue& value) = 0;
  // Sets the Enabled flag of the task at `path`; the task itself stays.
  [[nodiscard]] virtual Result<void> set_task_enabled(std::string_view path, bool enabled) = 0;
  // Sets the start type and the delayed-start flag of service `name`; trigger-start settings
  // stay as they are.
  [[nodiscard]] virtual Result<void> set_service_start(std::string_view name,
                                                       const ServiceStart& start) = 0;

 protected:
  IStartupSystem() = default;
};

}  // namespace myclean::startup
