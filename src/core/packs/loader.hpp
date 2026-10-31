#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/packs/schema.hpp"
#include "core/rules/data_files.hpp"

// Discovery and validation of knowledge packs (docs/packs.md). A pack is accepted or rejected
// whole: a rejected pack adds one error naming the pack, the file, the case and the field, and
// every other pack still loads.
namespace myclean::packs {

// rules::IDataFiles plus the folders of a directory: the pack folders of data/packs.
class IPackFiles : public rules::IDataFiles {
 public:
  // The folders directly inside `dir`, in any order. Fails with not_found when `dir` does not
  // exist.
  [[nodiscard]] virtual Result<std::vector<std::filesystem::path>> folders(
      const std::filesystem::path& dir) const = 0;
};

// IPackFiles over the real file system, with the portable C++ library.
class DiskPackFiles final : public IPackFiles {
 public:
  DiskPackFiles() = default;

  [[nodiscard]] Result<std::vector<std::filesystem::path>> list(
      const std::filesystem::path& dir) const override;
  [[nodiscard]] Result<std::string> read(const std::filesystem::path& file) const override;
  [[nodiscard]] Result<std::vector<std::filesystem::path>> folders(
      const std::filesystem::path& dir) const override;

 private:
  rules::DiskDataFiles files_;
};

// The pack in `folder`: pack.toml and its content files (clean.toml, attribution.toml,
// relocate.toml, compress.toml, kb.json, classes.toml; at least one). Fails with invalid_data,
// the message starting "pack '<folder name>': ", when
//   - the folder holds another file, the manifest's id is not the folder's name, or a content
//     file fails its schema;
//   - a case id is not [a-z0-9._-]; ids become "<pack>/<case>";
//   - an attribution area's reclaim ref (kind clean-rule) names no clean case of the pack. A
//     relocate ref naming a relocation recipe of the pack is namespaced; any other is kept as a
//     built-in relocation target (placement::CacheKind text, wsl, docker);
//   - classes.toml sets [caps] (job caps are not per pack);
//   - two cases of the pack claim the same thing (see load_packs).
// Fails with refused when the pack needs a newer MyClean than `running`.
[[nodiscard]] Result<Pack> load_pack(const rules::IDataFiles& files,
                                     const std::filesystem::path& folder, Origin origin,
                                     const Version& running);

enum class PackStatus {
  loaded,
  disabled,  // listed in the owner's packs.toml
  shadowed,  // an owner pack with the same id is loaded instead
  rejected,  // failed validation, needs a newer MyClean, or conflicts with a loaded pack
};

[[nodiscard]] std::string_view to_string(PackStatus status);

struct PackOutcome {
  std::string id;  // the folder's name
  std::filesystem::path folder;
  Origin origin = Origin::builtin;
  PackStatus status = PackStatus::loaded;
  std::string message;  // why it was rejected; empty otherwise
};

struct LoadReport {
  std::vector<Pack> packs;            // loaded: owner packs, then built-in ones, each by id
  std::vector<PackOutcome> outcomes;  // every pack folder, in the same order
  std::vector<Error> errors;          // every rejection, unreadable folder or settings file
};

// The owner's settings file in the override folder: <owner_dir>\packs.toml.
//   disabled = ["chrome", "ai-tools"]   # optional: ids of packs that are not loaded
inline constexpr std::string_view settings_file = "packs.toml";

// Loads the owner's packs (`owner_dir`, when given and present), then the built-in ones
// (`builtin_dir`). Precedence: a disabled id is skipped wherever it is; an owner pack shadows the
// built-in pack with its id (a rejected owner pack does not, the built-in one stays in effect).
// Conflicts: every case claims what it acts on (a clean target, native command or cleanup
// handler; an attribution path; a relocation setting; a compression path; a knowledge-base image
// and path; a class's criteria), compared case-insensitively. A pack claiming what a loaded pack
// already claims is rejected whole, naming both cases, so the owner's packs win over built-in
// ones and, within one origin, the first pack by id wins.
[[nodiscard]] LoadReport load_packs(const IPackFiles& files,
                                    const std::filesystem::path& builtin_dir,
                                    const std::optional<std::filesystem::path>& owner_dir,
                                    const Version& running);

}  // namespace myclean::packs
