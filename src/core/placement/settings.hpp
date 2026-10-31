#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

// The settings placement reads and changes, as interfaces to the OS. Implemented by
// src/platform/settings and, in tests, by fakes.
namespace myclean::placement {

// A user environment variable as stored: its unexpanded text and whether it is REG_EXPAND_SZ.
struct EnvValue {
  std::string text;  // UTF-8
  bool expandable = false;

  friend bool operator==(const EnvValue&, const EnvValue&) = default;
};

// The owner's environment variables (HKCU\Environment). Used in the owner's context (the tray),
// so %USERPROFILE% and friends expand to the owner's folders.
class IUserEnvironment {
 public:
  virtual ~IUserEnvironment() = default;
  IUserEnvironment(const IUserEnvironment&) = delete;
  IUserEnvironment& operator=(const IUserEnvironment&) = delete;
  IUserEnvironment(IUserEnvironment&&) = delete;
  IUserEnvironment& operator=(IUserEnvironment&&) = delete;

  // Variable `name`; nullopt when it is not set.
  [[nodiscard]] virtual Result<std::optional<EnvValue>> get(std::string_view name) const = 0;
  [[nodiscard]] virtual Result<void> set(std::string_view name, const EnvValue& value) = 0;
  // Deletes variable `name`; succeeds when it is not set.
  [[nodiscard]] virtual Result<void> remove(std::string_view name) = 0;
  // `text` with its %NAME% references expanded in the owner's environment.
  [[nodiscard]] virtual Result<std::filesystem::path> expand(std::string_view text) const = 0;
  // Tells running programs that the environment changed (WM_SETTINGCHANGE "Environment").
  [[nodiscard]] virtual Result<void> announce() = 0;

 protected:
  IUserEnvironment() = default;
};

// Whole small text files (tool configuration such as .npmrc), UTF-8.
class ITextFiles {
 public:
  virtual ~ITextFiles() = default;
  ITextFiles(const ITextFiles&) = delete;
  ITextFiles& operator=(const ITextFiles&) = delete;
  ITextFiles(ITextFiles&&) = delete;
  ITextFiles& operator=(ITextFiles&&) = delete;

  // The content of `path`; nullopt when the file does not exist.
  [[nodiscard]] virtual Result<std::optional<std::string>> read(
      const std::filesystem::path& path) const = 0;
  // Replaces the content of `path` (or creates it) in one step.
  [[nodiscard]] virtual Result<void> write(const std::filesystem::path& path,
                                           std::string_view text) = 0;
  // Deletes file `path`; succeeds when it does not exist.
  [[nodiscard]] virtual Result<void> remove(const std::filesystem::path& path) = 0;

 protected:
  ITextFiles() = default;
};

enum class RegistryType { string, expandable, dword };  // REG_SZ, REG_EXPAND_SZ, REG_DWORD

// A registry value as stored: a DWORD as its decimal text.
struct RegistryValue {
  std::string text;  // UTF-8
  RegistryType type = RegistryType::string;

  friend bool operator==(const RegistryValue&, const RegistryValue&) = default;
};

// String and DWORD values of the registry. A key is "HKCU\<path>" or "HKLM\<path>"; HKCU is the
// caller's, so relocation uses it in the owner's context (the tray).
class IRegistry {
 public:
  virtual ~IRegistry() = default;
  IRegistry(const IRegistry&) = delete;
  IRegistry& operator=(const IRegistry&) = delete;
  IRegistry(IRegistry&&) = delete;
  IRegistry& operator=(IRegistry&&) = delete;

  // Value `name` of `key`; nullopt when the key or the value does not exist. Other types fail.
  [[nodiscard]] virtual Result<std::optional<RegistryValue>> read(std::string_view key,
                                                                  std::string_view name) const = 0;
  // Sets value `name` of `key`, creating the key when it is missing.
  [[nodiscard]] virtual Result<void> write(std::string_view key, std::string_view name,
                                           const RegistryValue& value) = 0;
  // Deletes value `name` of `key`; succeeds when it does not exist.
  [[nodiscard]] virtual Result<void> remove(std::string_view key, std::string_view name) = 0;
  // The names of the subkeys of `key`; empty when it does not exist.
  [[nodiscard]] virtual Result<std::vector<std::string>> subkeys(std::string_view key) const = 0;

 protected:
  IRegistry() = default;
};

// What a tree holds: regular files and their logical bytes. Reparse points are not counted.
struct TreeTotals {
  std::uint64_t files = 0;
  Bytes bytes;

  friend bool operator==(const TreeTotals&, const TreeTotals&) = default;
};

// Copies folder trees (the "move" of a cache: copy, verify, then switch the setting). Copying
// deletes and moves nothing, so it does not go through SafeRemove; the old copy is removed later
// by a cleaning plan that does.
class ITreeCopier {
 public:
  virtual ~ITreeCopier() = default;
  ITreeCopier(const ITreeCopier&) = delete;
  ITreeCopier& operator=(const ITreeCopier&) = delete;
  ITreeCopier(ITreeCopier&&) = delete;
  ITreeCopier& operator=(ITreeCopier&&) = delete;

  // Copies the content of folder `from` into folder `to` (created when missing), keeping the
  // relative layout. Reparse points are neither followed nor copied, and an existing file in
  // `to` is never overwritten. Stops at the first failure.
  [[nodiscard]] virtual Result<TreeTotals> copy_tree(const std::filesystem::path& from,
                                                     const std::filesystem::path& to) = 0;

 protected:
  ITreeCopier() = default;
};

// One run of a Windows tool, shown to the owner exactly as text() before it runs.
struct Command {
  std::string tool;  // an executable of the Windows system folder: "powercfg.exe"
  std::vector<std::string> arguments;

  // "powercfg /h /type reduced": the tool without ".exe", then the arguments.
  [[nodiscard]] std::string text() const;

  friend bool operator==(const Command&, const Command&) = default;
};

struct CommandResult {
  std::uint32_t exit_code = 0;
  std::string output;  // standard output and error, UTF-8
};

// Runs Windows tools without a window. Only tools from the system folder are run.
class IProcessRunner {
 public:
  virtual ~IProcessRunner() = default;
  IProcessRunner(const IProcessRunner&) = delete;
  IProcessRunner& operator=(const IProcessRunner&) = delete;
  IProcessRunner(IProcessRunner&&) = delete;
  IProcessRunner& operator=(IProcessRunner&&) = delete;

  // Fails when the tool cannot be started or does not finish in time; a non-zero exit code is
  // a result, not a failure.
  [[nodiscard]] virtual Result<CommandResult> run(const Command& command) = 0;

 protected:
  IProcessRunner() = default;
};

enum class HibernationMode { off, reduced, full };

[[nodiscard]] std::string_view to_string(HibernationMode mode);
[[nodiscard]] std::optional<HibernationMode> parse_hibernation_mode(std::string_view text);

struct HibernationState {
  HibernationMode mode = HibernationMode::full;
  std::uint32_t size_percent = 0;  // of the memory; 0 = Windows' default for the mode
  std::filesystem::path file;      // "C:\hiberfil.sys"
  Bytes file_size;                 // zero when the file does not exist
  Bytes memory;                    // installed physical memory
};

// One page-file setting (Win32_PageFileSetting). Sizes in MB; 0 and 0 = system-managed size.
struct PageFile {
  std::filesystem::path path;  // "C:\pagefile.sys"
  std::uint32_t initial_mb = 0;
  std::uint32_t maximum_mb = 0;

  friend bool operator==(const PageFile&, const PageFile&) = default;
};

struct PageFileConfig {
  bool automatic = false;  // Win32_ComputerSystem.AutomaticManagedPagefile; `files` is then empty
  std::vector<PageFile> files;

  friend bool operator==(const PageFileConfig&, const PageFileConfig&) = default;
};

// A page file in use (Win32_PageFileUsage).
struct PageFileUse {
  std::filesystem::path path;
  Bytes allocated;
};

struct PageFileState {
  PageFileConfig config;
  std::vector<PageFileUse> in_use;
};

// The shadow-copy storage of one volume (Win32_ShadowStorage).
struct ShadowStorage {
  std::string volume;       // "C:", or the volume GUID path when it has no drive letter
  std::string diff_volume;  // where the storage lives, same form
  Bytes used;
  Bytes allocated;
  std::optional<Bytes> maximum;  // nullopt: unbounded
};

// The system settings that hold space on the system drive. Implemented by the service.
class ISystemSpace {
 public:
  virtual ~ISystemSpace() = default;
  ISystemSpace(const ISystemSpace&) = delete;
  ISystemSpace& operator=(const ISystemSpace&) = delete;
  ISystemSpace(ISystemSpace&&) = delete;
  ISystemSpace& operator=(ISystemSpace&&) = delete;

  [[nodiscard]] virtual Result<HibernationState> hibernation() const = 0;
  [[nodiscard]] virtual Result<PageFileState> page_files() const = 0;
  // Makes `config` the page-file configuration; it takes effect after a restart.
  [[nodiscard]] virtual Result<void> set_page_files(const PageFileConfig& config) = 0;
  // Every volume's shadow storage; empty when there is none.
  [[nodiscard]] virtual Result<std::vector<ShadowStorage>> shadow_storage() const = 0;

 protected:
  ISystemSpace() = default;
};

}  // namespace myclean::placement
