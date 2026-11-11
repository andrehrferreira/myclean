#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/startup/startup.hpp"

// The persistence points of the machine (security spec, "Persistence Point Inventory"): the
// startup items of core/startup (Run and RunOnce keys of both hives and views, Startup folders,
// logon and boot tasks, auto-start services) and the places programs start from that are no
// startup items: WMI event subscriptions, Image File Execution Options debuggers, the Winlogon
// Shell and Userinit values and AppInit_DLLs.
namespace myclean::security {

enum class PointSource {
  run,             // "run"
  run32,           // "run32"
  run_once,        // "runonce"
  run_once32,      // "runonce32"
  startup_folder,  // "folder"
  task,            // "task"
  service,         // "service"
  wmi,             // "wmi": a WMI event subscription of root\subscription
  ifeo,            // "ifeo": an Image File Execution Options Debugger value
  winlogon,        // "winlogon": the Winlogon Shell or Userinit value
  appinit,         // "appinit": a DLL of AppInit_DLLs
};

[[nodiscard]] std::string_view to_string(PointSource source);

// One __EventFilter bound to an __EventConsumer (__FilterToConsumerBinding), or a consumer with
// no binding (filter and query empty), which runs nothing until something binds it.
struct WmiSubscription {
  std::string filter;          // the filter's Name
  std::string query;           // the filter's WQL query
  std::string consumer_class;  // "CommandLineEventConsumer", "ActiveScriptEventConsumer", ...
  std::string consumer;        // the consumer's Name
  // CommandLineTemplate (else ExecutablePath) of a command-line consumer, ScriptFileName (else
  // the script text) of a script consumer; empty for the other classes.
  std::string command;
};

struct IfeoDebugger {
  bool wow64 = false;    // under WOW6432Node
  std::string image;     // the subkey: the program it applies to ("notepad.exe")
  std::string debugger;  // what starts instead of it
};

// Winlogon's Shell or Userinit of HKLM, or the Shell of the user's hive, which overrides it.
struct WinlogonValue {
  startup::Scope scope = startup::Scope::machine;
  std::string name;  // "Shell" or "Userinit"
  std::string value;
};

struct AppInit {
  bool wow64 = false;
  bool load_enabled = false;  // LoadAppInit_DLLs is 1
  std::string dlls;           // AppInit_DLLs as stored: names separated by spaces or commas
};

struct PersistenceSnapshot {
  std::vector<WmiSubscription> wmi;
  std::vector<IfeoDebugger> ifeo;
  std::vector<WinlogonValue> winlogon;
  std::vector<AppInit> appinit;
  std::string system_folder;  // "C:\Windows\System32": where Windows' own userinit.exe lives
  // A source that could not be read, with why ("WMI subscriptions: ..."); the others still are.
  std::vector<std::string> problems;
};

// The persistence sources that are not startup items, as the OS stores them. Implemented by
// src/platform/security; tests use fakes. Read-only.
class IPersistenceSystem {
 public:
  virtual ~IPersistenceSystem() = default;
  IPersistenceSystem(const IPersistenceSystem&) = delete;
  IPersistenceSystem& operator=(const IPersistenceSystem&) = delete;
  IPersistenceSystem(IPersistenceSystem&&) = delete;
  IPersistenceSystem& operator=(IPersistenceSystem&&) = delete;

  [[nodiscard]] virtual PersistenceSnapshot read() const = 0;

 protected:
  IPersistenceSystem() = default;
};

// The file a target names on this machine. Implemented by src/platform/security; tests use
// fakes.
class IPathResolver {
 public:
  virtual ~IPathResolver() = default;
  IPathResolver(const IPathResolver&) = delete;
  IPathResolver& operator=(const IPathResolver&) = delete;
  IPathResolver(IPathResolver&&) = delete;
  IPathResolver& operator=(IPathResolver&&) = delete;

  // `target` (Point::target) with its environment variables expanded for the owner, a bare file
  // name looked up where Windows would find it; nullopt when no such file exists.
  [[nodiscard]] virtual std::optional<std::filesystem::path> resolve(
      std::string_view target) const = 0;

 protected:
  IPathResolver() = default;
};

// What a persistence point is unusual for by its place alone, whatever it runs.
enum class Concern {
  winlogon_changed,  // a Winlogon value that is not Windows' default
  appinit_dll,       // a DLL loaded into every program that loads user32.dll
  ifeo_debugger,     // another program starts whenever the named one is started
  wmi_consumer,      // a WMI event starts a command or script
};

struct Point {
  PointSource source = PointSource::run;
  // Where it is stored: "HKCU\Software\Microsoft\Windows\CurrentVersion\Run", "Task Scheduler",
  // "HKLM\SYSTEM\CurrentControlSet\Services", "root\subscription", ...
  std::string location;
  std::string name;     // the value name, task path, service name, image, filter -> consumer
  std::string command;  // what it runs, as stored
  // The file it runs, as text (environment variables not expanded): the executable of the
  // command, the DLL rundll32.exe loads, a script file; empty when it runs no file (script text).
  std::string target;
  bool standard = false;  // Windows' own value: Winlogon defaults, empty AppInit_DLLs
  std::optional<bool> enabled;
  std::optional<std::string> signer;        // tasks and services, as core/startup read it
  std::optional<std::string> startup_item;  // the startup::Item id (list_startup "key")
  std::optional<Concern> concern;
};

// The file `command` runs: the quoted part, else the text up to the first ".exe" that ends a
// word (unquoted paths with spaces exist), else the first word; for rundll32.exe the DLL of its
// first argument (up to the comma). "\??\" is dropped and "\SystemRoot\" becomes "%SystemRoot%\".
[[nodiscard]] std::string target_of(std::string_view command);

// Whether `value` (Winlogon Userinit) is Windows' default: userinit.exe, bare or in
// `system_folder`, with or without the trailing comma.
[[nodiscard]] bool default_userinit(std::string_view value, std::string_view system_folder);

// Every persistence point: the startup items in their order, then WMI subscriptions, IFEO
// debuggers, Winlogon values and AppInit DLLs. Windows' default WMI subscription (the "SCM
// Event Log" filter bound to its NTEventLogEventConsumer), a Shell of explorer.exe, the default
// Userinit and an empty AppInit_DLLs are listed as standard; an AppInit_DLLs naming several
// DLLs gives one point per DLL.
[[nodiscard]] std::vector<Point> persistence_points(std::span<const startup::Item> items,
                                                    const PersistenceSnapshot& extra);

}  // namespace myclean::security
