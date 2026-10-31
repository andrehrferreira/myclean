#pragma once

#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>

#include "core/packs/loader.hpp"
#include "core/packs/registry.hpp"

namespace myclean::cli {

// The knowledge packs of an installed MyClean as this process sees them: the built-in packs of
// <data_dir>\packs and the owner's override packs (platform/packs: used only when administrators
// alone can write to them), and the registry of those whose applies_when holds for the user
// running this. Reads only.
struct LocalPacks {
  packs::LoadReport report;
  packs::Registry registry;
};

[[nodiscard]] LocalPacks load_local_packs(const std::filesystem::path& data_dir);

// `myclean packs ...`, run here without the service. `args` follow "packs":
//   list [--json] [--data <dir>]       every pack folder: id, version, origin, status, whether
//                                      it applies here, its case counts; rejections with why
//   validate [<path>] [--json] [--data <dir>]
//                                      validates a pack folder, or every pack of a folder of
//                                      packs (default <data>\packs, together, for conflicts),
//                                      and runs each pack's fixtures on an in-memory machine
// Returns exit_ok, exit_unavailable when a pack is rejected or a fixture fails (and for list,
// when the packs cannot be read), or exit_usage.
[[nodiscard]] int packs_command(std::span<const std::string> args, std::ostream& out,
                                std::ostream& err);

}  // namespace myclean::cli
