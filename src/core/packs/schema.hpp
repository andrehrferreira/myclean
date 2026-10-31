#pragma once

#include <compare>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/map/attribution.hpp"
#include "core/rules/schema.hpp"

// Knowledge packs (docs/packs.md): data/packs/<pack-id>/ holds a pack.toml manifest and one
// content file per case kind. This header holds what a loaded pack is; loader.hpp reads them.
namespace myclean::packs {

// MAJOR.MINOR.PATCH, each a non-negative integer.
struct Version {
  int major = 0;
  int minor = 0;
  int patch = 0;

  friend auto operator<=>(const Version&, const Version&) = default;
};

[[nodiscard]] std::optional<Version> parse_version(std::string_view text);
[[nodiscard]] std::string to_string(const Version& version);

// ---- pack.toml: the manifest ----------------------------------------------------------------
//
//   id          = "zig"                      # required: [a-z0-9-], the name of the pack's folder
//   version     = "1.0.0"                    # required: the pack's own version
//   title       = "Zig"                      # required
//   description = "..."                      # required
//   sources     = ["https://...", "observed on reference machine"]   # required
//   min_myclean = "0.1.0"                    # required: the oldest MyClean that can load it
//
//   [applies_when]                           # optional: absent = applies everywhere
//   all = [{ os_build_min = 19041 }]         # every condition holds, and
//   any = [{ path_exists = '%LOCALAPPDATA%\zig' }, { app_installed = '^Zig ' }]
//                                            # at least one does (when `any` is given)
//
// A condition is a table with exactly one of:
//   app_installed  Pattern over the display names of installed programs and Appx package names
//   path_exists    an absolute path pattern after %VARIABLE% expansion; '*' and '?' match within
//                  one component; false when a variable is not set
//   process_image  an image name ("ollama.exe") of a running process, any case
//   os_build_min   the lowest Windows build number

enum class ConditionKind { app_installed, path_exists, process_image, os_build_min };

struct Condition {
  ConditionKind kind = ConditionKind::path_exists;
  std::string text;                   // the app pattern's source, the path or the image name
  std::optional<rules::Pattern> app;  // app_installed only
  std::uint32_t build = 0;            // os_build_min only
};

struct Applicability {
  std::vector<Condition> all;
  std::vector<Condition> any;  // empty: no condition
};

struct Manifest {
  std::string id;
  Version version;
  std::string title;
  std::string description;
  std::vector<std::string> sources;
  Version min_myclean;
  Applicability applies_when;
};

// ---- relocate.toml: relocation recipes --------------------------------------------------------
//
//   [[relocate]]
//   id          = "rustup-home"                        # required, unique in the file
//   label       = "Rust toolchains (RUSTUP_HOME)"      # required
//   locations   = ['%USERPROFILE%\.rustup']             # required: where the content usually
//                                                      # is (absolute path patterns)
//   current     = { env = "RUSTUP_HOME" }              # required: where the current location is
//                                                      # read, exactly one of:
//                 { env = "NAME" }                     #   a user environment variable
//                 { config = 'path', key = "k", format = "ini" | "json" }
//                                                      #   a key of a tool's configuration file
//                 { registry = 'HKCU\...\Value', name = "Value" }
//                                                      #   a registry value; '*' in a key matches
//                                                      #   every subkey, each one an instance,
//                                                      #   named by its value `name` (optional)
//                                                      #   of the same key
//                 { command = "rustup show home" }     #   a command that prints the location
//                 where = { value = "Flags", mask = 8, equals = 8 }
//                                                      #   registry with name, optional: only the
//                                                      #   instances whose DWORD value `value`
//                                                      #   & mask == equals
//   target_kind = "env"                                # required: env | config | registry |
//                                                      # command | wsl-import; env, config and
//                                                      # registry write the setting `current`
//                                                      # names; wsl-import moves a WSL 1
//                                                      # distribution (instances of the Lxss key)
//                                                      # with wsl --export, a verified
//                                                      # wsl --import and wsl --unregister
//   command     = "wsl --manage {name} --move {target}"  # target_kind command only: {target} is
//                                                      # the new folder, {name} the instance name
//   content     = "move"                               # required: move | regenerate
//   needs       = ["rustup.exe", "cargo.exe"]          # optional: images that must not run
//   skip_names  = ["docker-desktop"]                   # optional: instances never offered
//   undo        = "restore"                            # required: restore (the previous value,
//                                                      # absence included; env, config and
//                                                      # registry), command (the move again with
//                                                      # {target} = the previous folder; command
//                                                      # and wsl-import) or none
//   note        = "..."                                # optional; required with undo = "none"

enum class SettingKind { env, config, registry, command, wsl_import };  // wsl_import: target only
enum class ContentMode { move, regenerate };  // text forms as placement::ContentMode
enum class UndoKind { restore, command, none };

[[nodiscard]] std::string_view to_string(SettingKind kind);
[[nodiscard]] std::string_view to_string(ContentMode mode);
[[nodiscard]] std::string_view to_string(UndoKind kind);
[[nodiscard]] std::optional<SettingKind> parse_setting_kind(std::string_view text);
[[nodiscard]] std::optional<ContentMode> parse_content_mode(std::string_view text);
[[nodiscard]] std::optional<UndoKind> parse_undo_kind(std::string_view text);

// The instances a registry setting offers: those whose DWORD value `value` (in the key of the
// setting's value), masked with `mask`, equals `equals`. An instance without it is not offered.
struct InstanceFilter {
  std::string value;
  std::uint32_t mask = 0;
  std::uint32_t equals = 0;
};

struct Setting {
  SettingKind kind = SettingKind::env;
  std::string text;    // the variable, the file, the registry value or the command
  std::string key;     // config: the key
  std::string format;  // config: ini (key = value lines, such as .npmrc) or json (a top-level key)
  std::string name;    // registry: the value naming each instance; empty when there is none
  std::optional<InstanceFilter> where;  // registry with name: the instances offered
};

struct RelocationRecipe {
  std::string id;  // "<pack>/<case>"
  std::string label;
  std::vector<std::string> locations;
  Setting current;
  SettingKind target_kind = SettingKind::env;
  std::string command;
  ContentMode content = ContentMode::move;
  std::vector<std::string> needs;
  std::vector<std::string> skip_names;
  UndoKind undo = UndoKind::restore;
  std::string note;
};

// ---- compress.toml: compression targets -------------------------------------------------------
//
//   [[compress]]
//   id        = "toolchains"                           # required, unique in the file
//   label     = "Rust toolchains"                      # required
//   paths     = ['%USERPROFILE%\.rustup\toolchains']   # required: absolute folder patterns
//   algorithm = "lzx"                                  # required hint: xpress4k | xpress8k |
//                                                      # xpress16k | lzx (compress::Algorithm)
//   exclude   = ["*.pdb"]                              # optional: names of files and folders
//                                                      # inside left alone ('*' and '?')
//   note      = "..."                                  # optional
//
// The compression analyzer's own exclusions (media, disk images, databases, recent files) apply
// on top.

struct CompressTarget {
  std::string id;  // "<pack>/<case>"
  std::string label;
  std::vector<std::string> paths;
  std::string algorithm;
  std::vector<std::string> exclude;
  std::string note;
};

// ---- a loaded pack ----------------------------------------------------------------------------

enum class Origin {
  builtin,  // data/packs, shipped with MyClean
  owner,    // %ProgramData%\MyClean\packs: the owner's override packs
};

[[nodiscard]] std::string_view to_string(Origin origin);

// Every case id is "<pack>/<case>". The other content files reuse the existing schemas:
// clean.toml (rules::CleaningRule, [[rule]]), attribution.toml (map::Area, [[area]]), kb.json
// (rules::KbEntry) and classes.toml (rules::ClassMatch, [[match]], without [caps]).
struct Pack {
  Manifest manifest;
  Origin origin = Origin::builtin;
  std::filesystem::path folder;
  std::vector<rules::CleaningRule> clean;
  std::vector<map::Area> attribution;
  std::vector<RelocationRecipe> relocate;
  std::vector<CompressTarget> compress;
  std::vector<rules::KbEntry> kb;
  std::vector<rules::ClassMatch> classes;
};

}  // namespace myclean::packs
