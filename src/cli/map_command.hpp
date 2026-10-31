#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>

namespace myclean::cli {

struct MapRequest {
  std::optional<std::string> target;  // a volume ("C:") or a folder, UTF-8; the system drive
  double min_gb = 1.0;                // GiB
  int depth = 6;
  std::optional<std::filesystem::path> data_dir;  // default: <executable>\..\..\data
  bool json = false;
};

// `myclean map`: scans the target here, in this process, at below-normal CPU priority and
// background I/O priority, then prints what each place is and how it can be reclaimed
// (core/map), from the attribution areas of the knowledge packs that apply here
// (load_local_packs). Progress and pack problems go to `err`. Reads only: nothing is written,
// followed or hydrated. Returns exit_ok, or exit_unavailable when no pack loads or the scan root
// fails.
[[nodiscard]] int map_command(const MapRequest& request, std::ostream& out, std::ostream& err);

// MyClean's data folder next to this executable: <executable>\..\..\data.
[[nodiscard]] std::filesystem::path default_data_dir();

}  // namespace myclean::cli
