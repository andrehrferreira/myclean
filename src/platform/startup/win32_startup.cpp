#include "platform/startup/win32_startup.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// clang-format off
#include <windows.h>  // first: the other Win32 headers need its types
#include <objbase.h>
#include <winsvc.h>
#include <KnownFolders.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <propvarutil.h>
#include <taskschd.h>
#include <wil/com.h>
#include <wil/resource.h>  // last: its COM and service helpers exist only when their headers came first
// clang-format on

#include "core/model/result.hpp"
#include "core/startup/startup.hpp"
#include "core/trust/trust.hpp"
#include "platform/settings/registry.hpp"
#include "platform/settings/win32_support.hpp"

namespace myclean::platform {

namespace startup_detail {
namespace {

char ascii_lower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_lower(a[i]) != ascii_lower(b[i])) {
      return false;
    }
  }
  return true;
}

bool starts_with_iequal(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && iequal(text.substr(0, prefix.size()), prefix);
}

// The first position at or after `from` where `needle` occurs in `text`, ignoring ASCII case.
std::size_t ifind(std::string_view text, std::string_view needle, std::size_t from) {
  for (std::size_t at = from; at + needle.size() <= text.size(); ++at) {
    if (iequal(text.substr(at, needle.size()), needle)) {
      return at;
    }
  }
  return std::string_view::npos;
}

bool blank(char c) {
  return c == ' ' || c == '\t';
}

}  // namespace

std::string executable_of(std::string_view command) {
  constexpr std::string_view blanks = " \t";
  constexpr std::string_view exe = ".exe";
  constexpr std::string_view nt_prefix = R"(\??\)";
  constexpr std::string_view system_root = R"(\SystemRoot\)";
  const std::size_t first = command.find_first_not_of(blanks);
  if (first == std::string_view::npos) {
    return {};
  }
  command.remove_prefix(first);
  std::string_view executable;
  if (command.front() == '"') {
    command.remove_prefix(1);
    executable = command.substr(0, command.find('"'));
  } else {
    executable = command.substr(0, command.find_first_of(blanks));
    for (std::size_t at = ifind(command, exe, 0); at != std::string_view::npos;
         at = ifind(command, exe, at + 1)) {
      const std::size_t end = at + exe.size();
      if (end == command.size() || blank(command[end])) {
        executable = command.substr(0, end);
        break;
      }
    }
  }
  if (starts_with_iequal(executable, nt_prefix)) {
    executable.remove_prefix(nt_prefix.size());
  }
  if (starts_with_iequal(executable, system_root)) {
    return R"(%SystemRoot%\)" + std::string(executable.substr(system_root.size()));
  }
  return std::string(executable);
}

bool is_svchost(std::string_view executable) {
  const std::size_t separator = executable.find_last_of(R"(\/)");
  const std::string_view name =
      separator == std::string_view::npos ? executable : executable.substr(separator + 1);
  return iequal(name, "svchost.exe");
}

std::string_view unquote(std::string_view text) {
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
    return text.substr(1, text.size() - 2);
  }
  return text;
}

std::string join_command(std::string_view path, std::string_view arguments) {
  std::string command(path);
  if (!arguments.empty()) {
    command.append(" ").append(arguments);
  }
  return command;
}

std::string expand_profile(std::string_view text, std::string_view profile) {
  constexpr std::string_view variable = "%USERPROFILE%";
  std::string result;
  std::size_t from = 0;
  for (std::size_t at = ifind(text, variable, 0); at != std::string_view::npos;
       at = ifind(text, variable, from)) {
    result.append(text.substr(from, at - from)).append(profile);
    from = at + variable.size();
  }
  result.append(text.substr(from));
  return result;
}

std::optional<startup::StartType> start_type_of(std::uint32_t code) {
  switch (code) {
    case SERVICE_BOOT_START:
      return startup::StartType::boot;
    case SERVICE_SYSTEM_START:
      return startup::StartType::system;
    case SERVICE_AUTO_START:
      return startup::StartType::automatic;
    case SERVICE_DEMAND_START:
      return startup::StartType::manual;
    case SERVICE_DISABLED:
      return startup::StartType::disabled;
    default:
      return std::nullopt;
  }
}

std::uint32_t start_type_code(startup::StartType type) {
  switch (type) {
    case startup::StartType::boot:
      return SERVICE_BOOT_START;
    case startup::StartType::system:
      return SERVICE_SYSTEM_START;
    case startup::StartType::automatic:
      return SERVICE_AUTO_START;
    case startup::StartType::manual:
      return SERVICE_DEMAND_START;
    case startup::StartType::disabled:
      return SERVICE_DISABLED;
  }
  return SERVICE_DEMAND_START;
}

bool delayed_flag(const startup::ServiceStart& start) {
  return start.type == startup::StartType::automatic && start.delayed;
}

}  // namespace startup_detail

namespace {

using settings_detail::narrow;
using settings_detail::widen;
using settings_detail::win32_error;
using startup::Approved;
using startup::ApprovedKey;
using startup::RunValue;
using startup::Scope;
using startup::Service;
using startup::StartupFile;
using startup::Task;
using startup_detail::executable_of;
using startup_detail::is_svchost;
using startup_detail::join_command;
using startup_detail::start_type_of;
using startup_detail::unquote;

constexpr std::wstring_view approved_root =
    LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\)";
constexpr std::size_t max_shell_link_text = 32768;  // a command line's limit, in characters
constexpr std::size_t service_buffer_bytes = std::size_t{64} * 1024;

// The Run and RunOnce keys of a hive. The WOW6432Node copies are read by path in the 64-bit view,
// never through the 32-bit view, so no value is listed twice.
struct RunKey {
  std::wstring_view path;
  bool wow64 = false;
  bool once = false;
};

constexpr std::array<RunKey, 4> run_keys{{
    {.path = LR"(Software\Microsoft\Windows\CurrentVersion\Run)"},
    {.path = LR"(Software\Microsoft\Windows\CurrentVersion\RunOnce)", .once = true},
    {.path = LR"(Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Run)", .wow64 = true},
    {.path = LR"(Software\WOW6432Node\Microsoft\Windows\CurrentVersion\RunOnce)",
     .wow64 = true,
     .once = true},
}};

constexpr std::array<std::pair<ApprovedKey, std::wstring_view>, 3> approved_subkeys{{
    {ApprovedKey::run, L"Run"},
    {ApprovedKey::run32, L"Run32"},
    {ApprovedKey::startup_folder, L"StartupFolder"},
}};

std::wstring_view approved_subkey(ApprovedKey key) {
  for (const auto& [candidate, name] : approved_subkeys) {
    if (candidate == key) {
      return name;
    }
  }
  return {};
}

tl::unexpected<Error> com_error(std::string_view what, HRESULT result) {
  Errc code = Errc::io_error;
  if (result == E_ACCESSDENIED || result == HRESULT_FROM_WIN32(ERROR_PRIVILEGE_NOT_HELD) ||
      result == HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED)) {
    code = Errc::access_denied;
  } else if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
             result == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
    code = Errc::not_found;
  }
  return fail(code,
              std::format("{} failed with 0x{:08X}", what, static_cast<std::uint32_t>(result)));
}

tl::unexpected<Error> service_error(std::string_view what, DWORD error) {
  if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
    return fail(Errc::not_found, std::string(what) + ": no such service");
  }
  return win32_error(what, error);
}

// COM for the calling thread for the lifetime of the scope, in whatever apartment the thread
// already has: a thread that is single-threaded already keeps its apartment (RPC_E_CHANGED_MODE)
// and is not uninitialized here.
class ComScope {
 public:
  ComScope() : result_(::CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE)) {}
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;
  ComScope(ComScope&&) = delete;
  ComScope& operator=(ComScope&&) = delete;
  ~ComScope() {
    if (SUCCEEDED(result_)) {
      ::CoUninitialize();
    }
  }

  [[nodiscard]] Result<void> status() const {
    if (FAILED(result_) && result_ != RPC_E_CHANGED_MODE) {
      return com_error("CoInitializeEx", result_);
    }
    return {};
  }

 private:
  HRESULT result_;
};

std::wstring expand(const std::wstring& text) {
  const DWORD length = ::ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
  if (length == 0) {
    return text;
  }
  std::wstring expanded(length, L'\0');
  const DWORD written = ::ExpandEnvironmentStringsW(text.c_str(), expanded.data(), length);
  if (written == 0 || written > length) {
    return text;
  }
  expanded.resize(written - 1);  // without the terminator
  return expanded;
}

std::wstring_view view_of(const wil::unique_bstr& text) {
  return {text.get(), ::SysStringLen(text.get())};
}

// The root of a hive and the prefix of every path in it: HKCU is HKEY_USERS\<sid> when a SID
// names the user.
struct Hive {
  HKEY root = nullptr;
  std::wstring prefix;
};

Hive hive_of(Scope scope, const std::string& user_sid) {
  if (scope == Scope::machine) {
    return {.root = HKEY_LOCAL_MACHINE};
  }
  if (user_sid.empty()) {
    return {.root = HKEY_CURRENT_USER};
  }
  return {.root = HKEY_USERS, .prefix = widen(user_sid) + L"\\"};
}

struct RawValue {
  std::wstring name;
  DWORD type = REG_NONE;
  std::vector<std::uint8_t> data;
};

// Every value of `path` under `root`, in the 64-bit view, names exactly as stored; none when the
// key does not exist.
Result<std::vector<RawValue>> values_of(HKEY root, const std::wstring& path) {
  wil::unique_hkey key;
  LSTATUS status =
      ::RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, key.put());
  if (status == ERROR_FILE_NOT_FOUND) {
    return std::vector<RawValue>{};
  }
  if (status != ERROR_SUCCESS) {
    return win32_error("opening " + narrow(path), static_cast<DWORD>(status));
  }
  for (;;) {  // again when a value grows between the size query and the enumeration
    DWORD count = 0;
    DWORD max_name = 0;
    DWORD max_data = 0;
    status = ::RegQueryInfoKeyW(key.get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                &count, &max_name, &max_data, nullptr, nullptr);
    if (status != ERROR_SUCCESS) {
      return win32_error("querying " + narrow(path), static_cast<DWORD>(status));
    }
    std::vector<RawValue> values;
    values.reserve(count);
    std::vector<wchar_t> name(static_cast<std::size_t>(max_name) + 1);
    std::vector<std::uint8_t> data(max_data);
    for (DWORD index = 0;; ++index) {
      auto name_length = static_cast<DWORD>(name.size());
      auto data_size = static_cast<DWORD>(data.size());
      DWORD type = REG_NONE;
      status = ::RegEnumValueW(key.get(), index, name.data(), &name_length, nullptr, &type,
                               data.data(), &data_size);
      if (status == ERROR_NO_MORE_ITEMS) {
        return values;
      }
      if (status == ERROR_MORE_DATA) {
        break;
      }
      if (status != ERROR_SUCCESS) {
        return win32_error("listing the values of " + narrow(path), static_cast<DWORD>(status));
      }
      values.push_back(RawValue{
          .name = std::wstring(name.data(), name_length),
          .type = type,
          .data = std::vector<std::uint8_t>(
              data.begin(), std::next(data.begin(), static_cast<std::ptrdiff_t>(data_size)))});
    }
  }
}

// REG_SZ or REG_EXPAND_SZ data as text, without its terminators.
std::wstring text_of(const std::vector<std::uint8_t>& data) {
  std::wstring text(data.size() / sizeof(wchar_t), L'\0');
  std::memcpy(text.data(), data.data(), text.size() * sizeof(wchar_t));
  while (!text.empty() && text.back() == L'\0') {
    text.pop_back();
  }
  return text;
}

Result<void> read_run(std::vector<RunValue>& run, Scope scope, const Hive& hive) {
  for (const RunKey& key : run_keys) {
    Result<std::vector<RawValue>> values =
        values_of(hive.root, hive.prefix + std::wstring(key.path));
    if (!values) {
      return tl::unexpected(values.error());
    }
    for (const RawValue& value : *values) {
      if (value.type != REG_SZ && value.type != REG_EXPAND_SZ) {
        continue;  // Explorer starts only string values
      }
      run.push_back(RunValue{.scope = scope,
                             .wow64 = key.wow64,
                             .once = key.once,
                             .name = narrow(value.name),
                             .command = narrow(text_of(value.data))});
    }
  }
  return {};
}

Result<void> read_approved(std::vector<Approved>& approved, Scope scope, const Hive& hive) {
  for (const auto& [key, subkey] : approved_subkeys) {
    Result<std::vector<RawValue>> values =
        values_of(hive.root, hive.prefix + std::wstring(approved_root) + std::wstring(subkey));
    if (!values) {
      return tl::unexpected(values.error());
    }
    for (RawValue& value : *values) {
      if (value.type == REG_BINARY) {
        approved.push_back(Approved{.scope = scope,
                                    .key = key,
                                    .name = narrow(value.name),
                                    .bytes = std::move(value.data)});
      }
    }
  }
  return {};
}

Result<std::filesystem::path> known_folder(REFKNOWNFOLDERID id, std::string_view what) {
  wil::unique_cotaskmem_string path;
  if (const HRESULT result = ::SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, path.put());
      FAILED(result)) {
    return com_error(std::string("locating the ") + std::string(what) + " Startup folder", result);
  }
  return std::filesystem::path{path.get()};
}

// The Startup folder of `user_sid`'s profile: its User Shell Folders entry (the default one when
// it has none), with %USERPROFILE% read from the ProfileList, not from this process.
Result<std::filesystem::path> user_startup_folder(const std::string& user_sid) {
  if (user_sid.empty()) {
    return known_folder(FOLDERID_Startup, "user");
  }
  const Win32Registry user{user_sid};
  const Result<std::optional<placement::RegistryValue>> folder = user.read(
      R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders)", "Startup");
  if (!folder) {
    return tl::unexpected(folder.error());
  }
  const Win32Registry machine;
  const Result<std::optional<placement::RegistryValue>> profile =
      machine.read(R"(HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\)" + user_sid,
                   "ProfileImagePath");
  if (!profile) {
    return tl::unexpected(profile.error());
  }
  const std::optional<placement::RegistryValue>& profile_path_value = *profile;
  if (!profile_path_value) {
    return fail(Errc::not_found, "no profile is registered for " + user_sid);
  }
  const std::string text =
      folder
          ->value_or(placement::RegistryValue{
              .text =
                  R"(%USERPROFILE%\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup)"})
          .text;
  const std::string profile_path = narrow(expand(widen(profile_path_value->text)));
  return std::filesystem::path{expand(widen(startup_detail::expand_profile(text, profile_path)))};
}

// A shortcut's target path and arguments, read without IShellLink::Resolve (which can search the
// disk and show UI); nullopt when the file does not load or names no file-system path.
std::optional<std::wstring> shortcut_target(const std::filesystem::path& file) {
  wil::com_ptr_nothrow<IShellLinkW> link;
  if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(link.put())))) {
    return std::nullopt;
  }
  const auto persist = link.try_query<IPersistFile>();
  if (!persist || FAILED(persist->Load(file.c_str(), STGM_READ))) {
    return std::nullopt;
  }
  std::vector<wchar_t> buffer(max_shell_link_text);
  if (link->GetPath(buffer.data(), static_cast<int>(buffer.size()), nullptr, 0) != S_OK ||
      buffer.front() == L'\0') {
    return std::nullopt;
  }
  std::wstring target{buffer.data()};
  buffer.front() = L'\0';
  if (SUCCEEDED(link->GetArguments(buffer.data(), static_cast<int>(buffer.size()))) &&
      buffer.front() != L'\0') {
    target.append(L" ").append(buffer.data());
  }
  return target;
}

bool is_shortcut(const std::filesystem::path& file) {
  const std::wstring extension = file.extension().native();
  return ::CompareStringOrdinal(extension.c_str(), -1, L".lnk", -1, TRUE) == CSTR_EQUAL;
}

// Every regular file of `folder`, hidden ones and non-shortcuts included; none when the folder
// does not exist.
Result<void> read_folder(std::vector<StartupFile>& files, Scope scope,
                         const std::filesystem::path& folder) {
  std::error_code error;
  if (!std::filesystem::exists(folder, error)) {
    return {};
  }
  std::filesystem::directory_iterator entry{folder, error};
  for (; !error && entry != std::filesystem::directory_iterator{}; entry.increment(error)) {
    std::error_code ignored;
    if (!entry->is_regular_file(ignored)) {
      continue;
    }
    const std::filesystem::path& file = entry->path();
    std::optional<std::wstring> target;
    if (is_shortcut(file)) {
      target = shortcut_target(file);
    }
    files.push_back(StartupFile{.scope = scope,
                                .file_name = narrow(file.filename().native()),
                                .target = narrow(target.value_or(file.native()))});
  }
  if (error) {
    return fail(Errc::io_error, "listing " + narrow(folder.native()) + ": " + error.message());
  }
  return {};
}

// The signer of each image, verified once per read.
class SignerCache {
 public:
  explicit SignerCache(const trust::IImageInspector& inspector) : inspector_(&inspector) {}

  [[nodiscard]] std::optional<std::string> signer(const std::filesystem::path& image) {
    if (image.empty()) {
      return std::nullopt;
    }
    const auto [found, inserted] = cache_.try_emplace(image.native());
    if (inserted) {
      const Result<trust::Signature> signature = inspector_->verify(image);
      if (signature && signature->status == trust::SignatureStatus::valid) {
        found->second = signature->signer;
      }
    }
    return found->second;
  }

 private:
  const trust::IImageInspector* inspector_;
  std::map<std::wstring, std::optional<std::string>> cache_;
};

std::wstring windows_folder(bool system) {
  std::array<wchar_t, MAX_PATH + 1> buffer{};
  const UINT length = system ? ::GetSystemDirectoryW(buffer.data(), MAX_PATH + 1)
                             : ::GetWindowsDirectoryW(buffer.data(), MAX_PATH + 1);
  if (length == 0 || length > MAX_PATH) {
    return {};
  }
  return {buffer.data(), length};
}

// The image of a task's exec action: its path expanded and unquoted; a bare file name ("rundll32")
// is looked up in System32, then in the Windows folder, as the Task Scheduler would find it.
std::filesystem::path task_image(std::wstring_view action_path) {
  std::filesystem::path image{expand(widen(unquote(narrow(action_path))))};
  if (image.empty() || image.has_parent_path()) {
    return image;
  }
  if (!image.has_extension()) {
    image += L".exe";
  }
  for (const bool system : {true, false}) {
    const std::wstring folder = windows_folder(system);
    std::error_code ignored;
    if (!folder.empty() && std::filesystem::is_regular_file(folder / image, ignored)) {
      return std::filesystem::path{folder} / image;
    }
  }
  return image;
}

VARIANT index_variant(LONG index) {
  VARIANT variant{};
  ::InitVariantFromInt32(index, &variant);
  return variant;
}

// Whether the definition has a logon trigger and a boot trigger; nullopt when it does not read.
std::optional<std::pair<bool, bool>> logon_and_boot(ITaskDefinition& definition) {
  wil::com_ptr_nothrow<ITriggerCollection> triggers;
  LONG count = 0;
  if (FAILED(definition.get_Triggers(triggers.put())) || FAILED(triggers->get_Count(&count))) {
    return std::nullopt;
  }
  std::pair<bool, bool> found{false, false};
  for (LONG i = 1; i <= count; ++i) {
    wil::com_ptr_nothrow<ITrigger> trigger;
    TASK_TRIGGER_TYPE2 type = TASK_TRIGGER_EVENT;
    if (FAILED(triggers->get_Item(i, trigger.put())) || FAILED(trigger->get_Type(&type))) {
      return std::nullopt;
    }
    found.first = found.first || type == TASK_TRIGGER_LOGON;
    found.second = found.second || type == TASK_TRIGGER_BOOT;
  }
  return found;
}

struct ExecAction {
  std::wstring path;
  std::wstring arguments;
};

// The first exec action (an empty path when there is none: a COM handler); nullopt when the
// actions do not read.
std::optional<ExecAction> first_exec(ITaskDefinition& definition) {
  wil::com_ptr_nothrow<IActionCollection> actions;
  LONG count = 0;
  if (FAILED(definition.get_Actions(actions.put())) || FAILED(actions->get_Count(&count))) {
    return std::nullopt;
  }
  for (LONG i = 1; i <= count; ++i) {
    wil::com_ptr_nothrow<IAction> action;
    TASK_ACTION_TYPE type = TASK_ACTION_COM_HANDLER;
    if (FAILED(actions->get_Item(i, action.put())) || FAILED(action->get_Type(&type))) {
      return std::nullopt;
    }
    if (type != TASK_ACTION_EXEC) {
      continue;
    }
    const auto exec = action.try_query<IExecAction>();
    wil::unique_bstr path;
    wil::unique_bstr arguments;
    if (!exec || FAILED(exec->get_Path(path.put())) ||
        FAILED(exec->get_Arguments(arguments.put()))) {
      return std::nullopt;
    }
    return ExecAction{.path = std::wstring(view_of(path)),
                      .arguments = std::wstring(view_of(arguments))};
  }
  return ExecAction{};
}

// The task, or nullopt when it does not read. Only logon and boot tasks get a signer: the others
// are not startup items.
std::optional<Task> task_of(IRegisteredTask& registered, SignerCache& signers) {
  wil::unique_bstr path;
  VARIANT_BOOL enabled = VARIANT_TRUE;
  wil::com_ptr_nothrow<ITaskDefinition> definition;
  if (FAILED(registered.get_Path(path.put())) || FAILED(registered.get_Enabled(&enabled)) ||
      FAILED(registered.get_Definition(definition.put()))) {
    return std::nullopt;
  }
  const std::optional<std::pair<bool, bool>> triggers = logon_and_boot(*definition);
  const std::optional<ExecAction> exec = first_exec(*definition);
  if (!triggers || !exec) {
    return std::nullopt;
  }
  Task task{.path = narrow(view_of(path)),
            .enabled = enabled != VARIANT_FALSE,
            .logon = triggers->first,
            .boot = triggers->second,
            .command = join_command(narrow(exec->path), narrow(exec->arguments))};
  if ((task.logon || task.boot) && !exec->path.empty()) {
    task.signer = signers.signer(task_image(exec->path));
  }
  return task;
}

void read_folder_tasks(ITaskFolder& folder, std::vector<Task>& tasks, SignerCache& signers) {
  wil::com_ptr_nothrow<IRegisteredTaskCollection> collection;
  LONG count = 0;
  if (FAILED(folder.GetTasks(TASK_ENUM_HIDDEN, collection.put())) ||
      FAILED(collection->get_Count(&count))) {
    return;  // a folder that does not read is skipped
  }
  tasks.reserve(tasks.size() + static_cast<std::size_t>(count));
  for (LONG i = 1; i <= count; ++i) {
    wil::com_ptr_nothrow<IRegisteredTask> registered;
    if (FAILED(collection->get_Item(index_variant(i), registered.put()))) {
      continue;
    }
    if (std::optional<Task> task = task_of(*registered, signers)) {
      tasks.push_back(std::move(*task));
    }
  }
}

// The root folder of the local Task Scheduler. Lives inside a ComScope.
Result<wil::com_ptr_nothrow<ITaskFolder>> task_root() {
  wil::com_ptr_nothrow<ITaskService> service;
  if (const HRESULT result = ::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                                IID_PPV_ARGS(service.put()));
      FAILED(result)) {
    return com_error("creating the Task Scheduler client", result);
  }
  const VARIANT none{};
  if (const HRESULT result = service->Connect(none, none, none, none); FAILED(result)) {
    return com_error("connecting to the Task Scheduler", result);
  }
  const wil::unique_bstr root_path{::SysAllocString(LR"(\)")};
  wil::com_ptr_nothrow<ITaskFolder> root;
  if (const HRESULT result = service->GetFolder(root_path.get(), root.put()); FAILED(result)) {
    return com_error("opening the root task folder", result);
  }
  return root;
}

// Every task of every folder, hidden ones included. Folders are walked with a stack, not by
// recursion.
Result<std::vector<Task>> read_tasks(SignerCache& signers) {
  Result<wil::com_ptr_nothrow<ITaskFolder>> root = task_root();
  if (!root) {
    return tl::unexpected(root.error());
  }
  std::vector<Task> tasks;
  std::vector<wil::com_ptr_nothrow<ITaskFolder>> pending{std::move(*root)};
  while (!pending.empty()) {
    const wil::com_ptr_nothrow<ITaskFolder> folder = std::move(pending.back());
    pending.pop_back();
    read_folder_tasks(*folder, tasks, signers);
    wil::com_ptr_nothrow<ITaskFolderCollection> children;
    LONG count = 0;
    if (FAILED(folder->GetFolders(0, children.put())) || FAILED(children->get_Count(&count))) {
      continue;
    }
    for (LONG i = 1; i <= count; ++i) {
      wil::com_ptr_nothrow<ITaskFolder> child;
      if (SUCCEEDED(children->get_Item(index_variant(i), child.put()))) {
        pending.push_back(std::move(child));
      }
    }
  }
  return tasks;
}

// The image whose signer stands for the service: the executable of its command line, or the
// ServiceDll of a service hosted in svchost.exe.
std::wstring service_binary(std::string_view name, const wchar_t* command) {
  std::wstring binary =
      expand(widen(executable_of(command != nullptr ? narrow(command) : std::string{})));
  if (!is_svchost(narrow(binary))) {
    return binary;
  }
  const Win32Registry machine;
  const Result<std::optional<placement::RegistryValue>> dll = machine.read(
      R"(HKLM\SYSTEM\CurrentControlSet\Services\)" + std::string(name) + R"(\Parameters)",
      "ServiceDll");
  const std::optional<placement::RegistryValue> service_dll =
      dll ? *dll : std::optional<placement::RegistryValue>{};
  if (service_dll && !service_dll->text.empty()) {
    return expand(widen(service_dll->text));
  }
  return binary;
}

// The service, or nullopt when its configuration does not read. Only automatic services get a
// signer: the others are not startup items.
std::optional<Service> service_of(SC_HANDLE manager, const ENUM_SERVICE_STATUS_PROCESSW& entry,
                                  SignerCache& signers) {
  const wil::unique_schandle handle{
      ::OpenServiceW(manager, entry.lpServiceName, SERVICE_QUERY_CONFIG)};
  if (!handle) {
    return std::nullopt;
  }
  DWORD needed = 0;
  ::QueryServiceConfigW(handle.get(), nullptr, 0, &needed);
  if (needed == 0) {
    return std::nullopt;
  }
  // operator new aligns the buffer for QUERY_SERVICE_CONFIGW.
  std::vector<std::byte> buffer(needed);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Win32 fills the struct
  auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
  if (::QueryServiceConfigW(handle.get(), config, needed, &needed) == FALSE) {
    return std::nullopt;
  }
  const std::optional<startup::StartType> type = start_type_of(config->dwStartType);
  if (!type) {
    return std::nullopt;
  }
  SERVICE_DELAYED_AUTO_START_INFO delayed{};
  DWORD size = 0;
  const bool is_delayed =
      ::QueryServiceConfig2W(
          handle.get(), SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Win32 API
          reinterpret_cast<LPBYTE>(&delayed), sizeof(delayed), &size) != FALSE &&
      delayed.fDelayedAutostart != FALSE;
  Service service{.name = narrow(entry.lpServiceName),
                  .display_name = narrow(entry.lpDisplayName),
                  .start = startup::ServiceStart{.type = *type, .delayed = is_delayed}};
  const std::wstring binary = service_binary(service.name, config->lpBinaryPathName);
  service.binary = narrow(binary);
  if (*type == startup::StartType::automatic) {
    service.signer = signers.signer(binary);
  }
  return service;
}

Result<std::vector<Service>> read_services(SignerCache& signers) {
  const wil::unique_schandle manager{
      ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE)};
  if (!manager) {
    return win32_error("opening the service manager", ::GetLastError());
  }
  std::vector<Service> services;
  // operator new aligns the buffer for ENUM_SERVICE_STATUS_PROCESSW.
  std::vector<BYTE> buffer(service_buffer_bytes);
  DWORD resume = 0;
  for (;;) {
    DWORD needed = 0;
    DWORD returned = 0;
    const BOOL done = ::EnumServicesStatusExW(
        manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buffer.data(),
        static_cast<DWORD>(buffer.size()), &needed, &returned, &resume, nullptr);
    const DWORD error = done != FALSE ? ERROR_SUCCESS : ::GetLastError();
    if (error != ERROR_SUCCESS && error != ERROR_MORE_DATA) {
      return win32_error("listing the services", error);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Win32 fills an array of them
    const auto* first = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
    for (const ENUM_SERVICE_STATUS_PROCESSW& entry : std::span{first, returned}) {
      if (std::optional<Service> service = service_of(manager.get(), entry, signers)) {
        services.push_back(std::move(*service));
      }
    }
    if (error == ERROR_SUCCESS) {
      return services;
    }
    if (needed > buffer.size()) {
      buffer.resize(needed);
    }
  }
}

}  // namespace

Result<startup::Snapshot> Win32StartupSystem::read() const {
  const ComScope com;
  if (Result<void> status = com.status(); !status) {
    return tl::unexpected(status.error());
  }
  startup::Snapshot snapshot;
  for (const Scope scope : {Scope::user, Scope::machine}) {
    const Hive hive = hive_of(scope, user_sid_);
    if (Result<void> run = read_run(snapshot.run, scope, hive); !run) {
      return tl::unexpected(run.error());
    }
    if (Result<void> approved = read_approved(snapshot.approved, scope, hive); !approved) {
      return tl::unexpected(approved.error());
    }
  }

  Result<std::filesystem::path> user_folder = user_startup_folder(user_sid_);
  if (!user_folder) {
    return tl::unexpected(user_folder.error());
  }
  Result<std::filesystem::path> common_folder = known_folder(FOLDERID_CommonStartup, "common");
  if (!common_folder) {
    return tl::unexpected(common_folder.error());
  }
  if (Result<void> files = read_folder(snapshot.files, Scope::user, *user_folder); !files) {
    return tl::unexpected(files.error());
  }
  if (Result<void> files = read_folder(snapshot.files, Scope::machine, *common_folder); !files) {
    return tl::unexpected(files.error());
  }

  SignerCache signers{inspector_};
  Result<std::vector<Task>> tasks = read_tasks(signers);
  if (!tasks) {
    return tl::unexpected(tasks.error());
  }
  snapshot.tasks = std::move(*tasks);
  Result<std::vector<Service>> services = read_services(signers);
  if (!services) {
    return tl::unexpected(services.error());
  }
  snapshot.services = std::move(*services);
  return snapshot;
}

Result<void> Win32StartupSystem::set_approved(Scope scope, ApprovedKey key, std::string_view name,
                                              const startup::ApprovedValue& value) {
  const Hive hive = hive_of(scope, user_sid_);
  const std::wstring path =
      hive.prefix + std::wstring(approved_root) + std::wstring(approved_subkey(key));
  const std::wstring value_name = widen(name);
  const std::string what =
      R"(StartupApproved\)" + narrow(approved_subkey(key)) + "\\" + std::string(name);
  wil::unique_hkey handle;
  if (!value) {
    LSTATUS status =
        ::RegOpenKeyExW(hive.root, path.c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, handle.put());
    if (status == ERROR_FILE_NOT_FOUND) {
      return {};
    }
    if (status != ERROR_SUCCESS) {
      return win32_error("opening " + what, static_cast<DWORD>(status));
    }
    status = ::RegDeleteValueW(handle.get(), value_name.c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
      return win32_error("deleting " + what, static_cast<DWORD>(status));
    }
    return {};
  }
  LSTATUS status =
      ::RegCreateKeyExW(hive.root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY,
                        nullptr, handle.put(), nullptr);
  if (status != ERROR_SUCCESS) {
    return win32_error("opening " + what, static_cast<DWORD>(status));
  }
  status = ::RegSetValueExW(handle.get(), value_name.c_str(), 0, REG_BINARY, value->data(),
                            static_cast<DWORD>(value->size()));
  if (status != ERROR_SUCCESS) {
    return win32_error("writing " + what, static_cast<DWORD>(status));
  }
  return {};
}

Result<void> Win32StartupSystem::set_task_enabled(std::string_view path, bool enabled) {
  const ComScope com;
  if (Result<void> status = com.status(); !status) {
    return status;
  }
  Result<wil::com_ptr_nothrow<ITaskFolder>> root = task_root();
  if (!root) {
    return tl::unexpected(root.error());
  }
  const std::string what = "task " + std::string(path);
  const wil::unique_bstr task_path{::SysAllocString(widen(path).c_str())};
  wil::com_ptr_nothrow<IRegisteredTask> task;
  if (const HRESULT result = (*root)->GetTask(task_path.get(), task.put()); FAILED(result)) {
    return com_error("opening " + what, result);
  }
  if (const HRESULT result = task->put_Enabled(enabled ? VARIANT_TRUE : VARIANT_FALSE);
      FAILED(result)) {
    return com_error(std::string(enabled ? "enabling " : "disabling ") + what, result);
  }
  return {};
}

Result<void> Win32StartupSystem::set_service_start(std::string_view name,
                                                   const startup::ServiceStart& start) {
  const wil::unique_schandle manager{::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
  if (!manager) {
    return win32_error("opening the service manager", ::GetLastError());
  }
  const std::string what = "service " + std::string(name);
  const wil::unique_schandle service{
      ::OpenServiceW(manager.get(), widen(name).c_str(), SERVICE_CHANGE_CONFIG)};
  if (!service) {
    return service_error("opening " + what, ::GetLastError());
  }
  // Only the start type changes; trigger-start settings are another configuration level and
  // stay as they are.
  if (::ChangeServiceConfigW(service.get(), SERVICE_NO_CHANGE,
                             startup_detail::start_type_code(start.type), SERVICE_NO_CHANGE,
                             nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                             nullptr) == FALSE) {
    return service_error("setting the start type of " + what, ::GetLastError());
  }
  SERVICE_DELAYED_AUTO_START_INFO delayed{.fDelayedAutostart =
                                              startup_detail::delayed_flag(start) ? TRUE : FALSE};
  if (::ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed) ==
      FALSE) {
    return service_error("setting the delayed start of " + what, ::GetLastError());
  }
  return {};
}

}  // namespace myclean::platform
