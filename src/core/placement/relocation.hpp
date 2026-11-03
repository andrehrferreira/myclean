#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/settings.hpp"
#include "core/telemetry/process_table.hpp"

// Relocation of TEMP/TMP and the package caches that keep growing on the system drive, through
// each tool's supported setting (never a junction), with exact undo
// (docs/analysis/myclean-planning/02-disk-cleaning-and-relocation.md, F-014).
namespace myclean::placement {

enum class CacheKind {
  temp,        // user TEMP and TMP
  gradle,      // GRADLE_USER_HOME
  cargo,       // CARGO_HOME, plus the user PATH entry for its bin folder
  npm,         // `npm config set cache` (user .npmrc), or npm_config_cache when that is set
  nuget,       // NUGET_PACKAGES
  pip,         // PIP_CACHE_DIR
  go_modules,  // GOMODCACHE
  go_build,    // GOCACHE
  unreal_ddc,  // UE-LocalDataCachePath (Unreal Engine local derived data cache)
};

inline constexpr std::array<CacheKind, 9> all_cache_kinds = {
    CacheKind::temp,       CacheKind::gradle,   CacheKind::cargo,
    CacheKind::npm,        CacheKind::nuget,    CacheKind::pip,
    CacheKind::go_modules, CacheKind::go_build, CacheKind::unreal_ddc};

[[nodiscard]] std::string_view to_string(CacheKind kind);
[[nodiscard]] std::optional<CacheKind> parse_cache_kind(std::string_view text);

// What happens to the content already in the old folder.
enum class ContentMode {
  move,        // copied to the target and verified before the setting switches
  regenerate,  // left behind; the tool rebuilds what it needs at the target
};

[[nodiscard]] std::string_view to_string(ContentMode mode);
[[nodiscard]] std::optional<ContentMode> parse_content_mode(std::string_view text);

// What relocation works with. Every pointer must outlive its users.
struct Tools {
  IUserEnvironment* environment = nullptr;
  ITextFiles* files = nullptr;
  ITreeCopier* copier = nullptr;
  os::IFileSystem* file_system = nullptr;  // sizes, copy verification, target folder creation
  const IDriveFacts* drives = nullptr;
  IRegistry* registry = nullptr;                  // recipes only (recipe.hpp)
  telemetry::IProcessTable* processes = nullptr;  // recipes only: their `needs`
};

// Where one cache is now.
struct CacheLocation {
  CacheKind kind = CacheKind::temp;
  std::string setting;  // what relocates it: "TEMP, TMP", "CARGO_HOME, PATH", "npm config cache"
  std::filesystem::path path;  // expanded and normalized
  bool configured = false;     // set explicitly; false when it is the tool's default location
  Bytes size;                  // size on disk; zero when the folder does not exist
};

// The current location of `kind`: its setting, or the tool's default when it is not set.
[[nodiscard]] Result<CacheLocation> locate(const Tools& tools, CacheKind kind);

// Cargo's installed tools (rustup's proxies among them) live in CARGO_HOME\bin, so its content
// must move; every other cache regenerates.
[[nodiscard]] ContentMode recommended_content(CacheKind kind);

// The folder `kind` gets on the volume with root `root`: "<root>Temp" for TEMP,
// "<root>DevCaches\<name>" for a cache ("D:\DevCaches\npm-cache").
[[nodiscard]] std::filesystem::path target_folder(CacheKind kind,
                                                  const std::filesystem::path& root);

// One row of the Relocate view.
struct RelocationProposal {
  CacheLocation current;
  std::optional<std::filesystem::path> target;  // nullopt when no relocation is proposed
  std::string reason;                           // why there is no target; empty otherwise
  ContentMode content = ContentMode::regenerate;
};

// Every cache with its location and size. A cache on the system drive gets a target on the
// best volume for its size (best_target: healthy, room, drive class), with
// recommended_content(); one elsewhere, or without a suitable volume, gets the reason instead.
[[nodiscard]] Result<std::vector<RelocationProposal>> propose_relocations(const Tools& tools);

// What the owner approved.
struct Relocation {
  CacheKind kind = CacheKind::temp;
  std::filesystem::path target;
  ContentMode content = ContentMode::regenerate;
};

// Relocates one cache (tier ask, run in the owner's context):
//   preview  one item: the current folder, its identity and size;
//   apply    refuses when the location changed since the preview, the target overlaps it, its
//            drive is not healthy or lacks room (unsuitable()), or Cargo is to regenerate;
//            "move" copies the content and checks the copy holds the same files and bytes
//            before any setting changes; then writes every setting, restoring those already
//            written when one fails, and announces the environment change (best effort: a
//            program that misses it sees the change at its next start). The undo data holds
//            each setting's previous and new value, absence included;
//   verify   metric "relocated_settings": how many settings hold the new value (better when all);
//   undo     restores every previous value exactly (deleting a variable that was not set), and
//            refuses, changing nothing, when a setting was changed by someone else meanwhile.
// The content left in the old folder is offered for cleaning with leftover_plan().
class RelocateAction final : public Action {
 public:
  // `tools` must outlive the action.
  RelocateAction(const Tools& tools, Relocation relocation)
      : tools_(&tools), relocation_(std::move(relocation)) {}

  [[nodiscard]] ActionInfo describe() const override;
  [[nodiscard]] Result<Preview> preview() override;
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

 private:
  const Tools* tools_;
  Relocation relocation_;
};

// Size on disk of what can be read below `root`: a subfolder that cannot be listed (another
// user's or a locked one, common in TEMP) is left out, since the size is shown as an estimate.
// Zero when `root` does not exist.
[[nodiscard]] Result<Bytes> readable_size(const os::IFileSystem& file_system,
                                          const std::filesystem::path& root);

// What the tree below `root` holds: regular files and their logical bytes, reparse points not
// followed nor counted. Fails when a folder cannot be listed.
[[nodiscard]] Result<TreeTotals> tree_totals(const os::IFileSystem& file_system,
                                             const std::filesystem::path& root);

// Why `target` cannot take `needed` bytes, as a refusal; success when its drive is healthy and
// has room (unsuitable()).
[[nodiscard]] Result<void> check_target(const IDriveFacts& drives,
                                        const std::filesystem::path& target, Bytes needed);

// Copies `source` into the empty or missing folder `target` and checks that the copy holds the
// same files and bytes; `label` names the content in the messages.
[[nodiscard]] Result<void> copy_content(const Tools& tools, const std::filesystem::path& source,
                                        const std::filesystem::path& target,
                                        std::string_view label);

// The old folder of an applied relocation (from its undo data) as a "QuarantineItems" plan,
// for the owner to approve like any cleaning item. Fails with not_found when it is gone.
[[nodiscard]] Result<Plan> leftover_plan(const os::IFileSystem& file_system, const Undo& undo,
                                         const Actor& proposer, WallTime now);

}  // namespace myclean::placement
