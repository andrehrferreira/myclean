#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/tray_model/cleaning.hpp"
#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/governor.hpp"
#include "core/tray_model/poller.hpp"
#include "core/tray_model/replies.hpp"
#include "core/tray_model/screens.hpp"
#include "core/tray_model/space_map.hpp"

namespace myclean::tray {

enum class Section {
  cpu_memory,
  disks,
  volumes,
  processes,
  alerts,
  freezes,
  cleanup,
  space,
  governor,
  placement,
  optimize,
  startup,
  apps,
  security,
  backups,
  audit,
  settings,
  reports
};

// What the owner chose in the dashboard; kept between frames.
struct UiState {
  Section section = Section::cpu_memory;
  tray_model::ProcessSort sort = tray_model::ProcessSort::cpu;
  tray_model::Range range = tray_model::Range::live;
  // The process the owner asked to explain; the explanation panel shows while it is set.
  std::optional<std::uint32_t> explained;
  // The plans whose details are shown, and the items the owner ticked in each (none at first).
  std::set<std::int64_t> open_plans;
  std::map<std::int64_t, std::set<std::int64_t>> selected;
  // The quarantine run whose "Delete now" waits for the owner's confirmation.
  std::optional<std::int64_t> confirm_purge;
  // The Space list shown, the volume of its largest items, and the lists (with their volume)
  // already asked for, so that showing one loads it once.
  tray_model::SpaceList space = tray_model::SpaceList::top;
  std::string space_volume;
  std::set<std::string> space_asked;
  // The folders opened in the treemap of the largest items, below space_volume.
  tray_model::FolderTrail space_trail;
  // The owner-decision candidates the owner clicked: their row says the plan started instead of
  // offering the button again.
  std::set<std::int64_t> acted;
  // The governor: the minutes a pause lasts, and the tree whose Kill waits for the owner's
  // confirmation.
  int pause_minutes = 60;
  std::optional<tray_model::GovernorProcess> confirm_kill;
  // The screens already loaded once (each loads when first shown), and the startup items the
  // owner ticked, by key.
  std::set<tray_model::Screen> screens_asked;
  std::set<std::string> startup_ticked;
  // Apps: only those not used for 6 months.
  bool apps_unused_only = false;
  // Audit: the "before" of each older page shown, the current one last (empty: the newest page).
  std::vector<std::int64_t> audit_pages;
  // Reports: the two snapshots the owner picked to compare.
  std::string compare_from;
  std::string compare_to;
  // What the owner asked of the service during the frame; the window sends and clears them.
  std::vector<tray_model::Request> requests;
};

// The dark theme of the dashboard at `scale` (1.0 at 96 DPI): colors, spacing and the font size
// of ImGui and ImPlot. Needs both contexts current; call again when the DPI changes.
void apply_style(float scale);

// Draws the whole dashboard into the current ImGui frame (between NewFrame and Render), filling
// the main viewport. Local times use `zone`. Needs an ImPlot context.
void draw_dashboard(const tray_model::DashboardModel& model, const std::chrono::time_zone& zone,
                    UiState& state);

}  // namespace myclean::tray
