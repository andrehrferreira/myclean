#pragma once

#include <iosfwd>
#include <span>
#include <string>

namespace myclean::cli {

// `myclean scan-all [--drives C:,D:,...] [--budget-minutes N] [--data <dir>] [--json]`, with
// `args` the arguments after "scan-all". Scans every fixed volume with a drive letter (or the
// ones listed) here, in this process, in Windows background mode at below-normal priority:
// builds each volume's tree (from the NTFS index when elevated, else scanner v1, stopped after
// N minutes, default 10), then maps it, evaluates the cleaning rules and developer artifacts,
// estimates compression, analyses drive maintenance, finds cold data, placement advice and
// cross-drive duplicates (within N minutes and 8 GiB hashed), and prints one report grouped by
// action (core/scanall). Progress goes to `err`. Reads only: nothing is deleted, moved,
// compressed or changed. Returns exit_ok, exit_usage for bad arguments, or exit_unavailable when
// the volumes cannot be listed.
[[nodiscard]] int scan_all_command(std::span<const std::string> args, std::ostream& out,
                                   std::ostream& err);

}  // namespace myclean::cli
