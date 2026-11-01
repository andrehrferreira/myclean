#include "tray/inbox_ui.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/tray_model/cleaning.hpp"
#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/format.hpp"
#include "core/tray_model/poller.hpp"
#include "core/tray_model/snapshot.hpp"
#include "tray/dashboard_ui.hpp"
#include "tray/ui_text.hpp"

namespace myclean::tray {

namespace {

namespace color = ui::color;
using tray_model::CleanupView;
using tray_model::DashboardModel;
using tray_model::PlanDetail;
using tray_model::PlanRow;
using ui::colored_text;
using ui::local_date_time;
using ui::muted_text;
using ui::text;
using ui::wrapped_text;

constexpr ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                        ImGuiTableFlags_SizingStretchProp |
                                        ImGuiTableFlags_PadOuterX;

std::string size_text(Bytes bytes) {
  return tray_model::format_bytes(static_cast<double>(bytes.count()));
}

std::string plural(std::int64_t count, std::string_view noun) {
  return std::format("{} {}{}", count, noun, count == 1 ? "" : "s");
}

std::string totals_text(const tray_model::ByteTotals& totals) {
  std::string line;
  for (const auto& [name, bytes] : totals) {
    line += (line.empty() ? "" : ", ") + name + " " + size_text(bytes);
  }
  return line;
}

// Who runs a plan, in the owner's words.
std::string_view runner(const PlanRow& plan) {
  return plan.scope == "user" ? "the tray, under your account" : "the MyClean service";
}

unsigned state_color(std::string_view state) {
  if (state == "verified" || state == "applied") {
    return color::good;
  }
  if (state == "failed") {
    return color::bad;
  }
  if (state == tray_model::awaiting_approval) {
    return color::warning;
  }
  return color::muted;
}

void draw_scan(const CleanupView& cleanup, const std::chrono::time_zone& zone, bool reachable,
               UiState& state) {
  const auto& status = cleanup.status();
  const bool scanning = status && status->scanning;
  ImGui::BeginDisabled(!reachable || scanning);
  if (ImGui::Button("Scan now")) {
    state.requests.emplace_back(tray_model::ScanNow{});
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (!status) {
    muted_text("Scan status: " + std::string(tray_model::not_available));
    return;
  }
  if (scanning) {
    const auto done = static_cast<float>(std::clamp(status->progress, 0.0, 1.0));
    ImGui::ProgressBar(done, ImVec2(-1.0F, 0.0F),
                       std::format("scanning, {:.0f} %", done * 100.0F).c_str());
  } else if (!status->last_completed) {
    muted_text("No scan has run yet.");
  } else {
    muted_text(std::format("Last scan {}: {}, {} reclaimable",
                           local_date_time(zone, *status->last_completed),
                           plural(status->candidates, "candidate"), size_text(status->bytes)));
  }
  if (status->next_auto_run) {
    muted_text("Next automatic run: " + local_date_time(zone, *status->next_auto_run));
  }
  if (!status->unavailable.empty()) {
    wrapped_text(color::warning, "Cleaning cannot run now: " + status->unavailable);
  }
}

void draw_task(const CleanupView& cleanup) {
  if (!cleanup.task_activity().empty()) {
    colored_text(color::accent, cleanup.task_activity());
  }
  if (!cleanup.task_note().empty()) {
    wrapped_text(color::muted, "Last task: " + cleanup.task_note());
  }
}

// Sends OpenPlan or ClosePlan when the owner opens or closes a plan; UiState::open_plans then
// follows.
void track_open(std::int64_t id, bool open, UiState& state) {
  if (open && state.open_plans.insert(id).second) {
    state.requests.emplace_back(tray_model::OpenPlan{.id = id});
  } else if (!open && state.open_plans.erase(id) > 0) {
    state.requests.emplace_back(tray_model::ClosePlan{.id = id});
  }
}

// The items of a plan; while it waits, each with the owner's tick.
void draw_items(const PlanDetail& detail, bool waiting, std::set<std::int64_t>& ticked) {
  if (!ImGui::BeginTable("items", 4, table_flags)) {
    return;
  }
  ImGui::TableSetupColumn(waiting ? "Approve" : "Decision", ImGuiTableColumnFlags_WidthStretch,
                          0.7F);
  ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch, 4.0F);
  ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.8F);
  ImGui::TableSetupColumn("Why", ImGuiTableColumnFlags_WidthStretch, 2.4F);
  ImGui::TableHeadersRow();
  for (const tray_model::PlanItemRow& item : detail.items) {
    ImGui::TableNextRow();
    ImGui::PushID(std::to_string(item.id).c_str());
    ImGui::TableNextColumn();
    if (waiting) {
      bool on = ticked.contains(item.id);
      if (ImGui::Checkbox("##pick", &on)) {
        if (on) {
          ticked.insert(item.id);
        } else {
          ticked.erase(item.id);
        }
      }
    } else if (item.approved) {
      colored_text(*item.approved ? color::good : color::muted,
                   *item.approved ? "approved" : "not approved");
    } else {
      muted_text("undecided");
    }
    ImGui::TableNextColumn();
    wrapped_text(color::text, item.path);
    ImGui::TableNextColumn();
    text(size_text(item.bytes));
    ImGui::TableNextColumn();
    wrapped_text(color::muted, item.rule ? *item.rule + ": " + item.reason : item.reason);
    ImGui::PopID();
  }
  ImGui::EndTable();
  if (static_cast<std::int64_t>(detail.items.size()) < detail.plan.items) {
    muted_text(std::format("and {} more not listed",
                           detail.plan.items - static_cast<std::int64_t>(detail.items.size())));
  }
}

void draw_decision(const PlanDetail& detail, bool reachable, UiState& state) {
  std::set<std::int64_t>& ticked = state.selected[detail.plan.id];
  ImGui::BeginDisabled(!reachable);
  const std::string all = std::format("Approve all ({}, {})", plural(detail.plan.items, "item"),
                                      size_text(detail.plan.bytes));
  if (ImGui::Button(all.c_str())) {
    state.requests.emplace_back(
        tray_model::ApprovePlan{.id = detail.plan.id, .selection = tray_model::select_all()});
    ticked.clear();
  }
  ImGui::SameLine();
  const Result<tray_model::Selection> some = tray_model::select_items(detail, ticked);
  ImGui::BeginDisabled(!some);
  if (ImGui::Button(std::format("Approve selected ({})", ticked.size()).c_str()) && some) {
    state.requests.emplace_back(tray_model::ApprovePlan{.id = detail.plan.id, .selection = *some});
    ticked.clear();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Reject")) {
    state.requests.emplace_back(tray_model::RejectPlan{.id = detail.plan.id});
    ticked.clear();
  }
  ImGui::EndDisabled();
  if (!some && !ticked.empty()) {
    wrapped_text(color::warning, some.error().message);
  }
}

void draw_detail(const PlanDetail& detail, bool reachable, UiState& state) {
  const PlanRow& plan = detail.plan;
  const bool waiting = tray_model::pending(plan);
  if (!detail.outcome.empty()) {
    wrapped_text(state_color(plan.state), detail.outcome);
  }
  if (detail.freed) {
    colored_text(color::good, "Freed " + size_text(*detail.freed));
  }
  for (const std::string& warning : detail.warnings) {
    wrapped_text(color::warning, "Risk: " + warning);
  }
  if (!detail.commands.empty()) {
    muted_text("Runs exactly:");
    for (const std::string& command : detail.commands) {
      wrapped_text(color::accent, "  " + command);
    }
  }
  wrapped_text(color::muted,
               "Undo: " + (detail.undo.empty() ? std::string("not possible") : detail.undo));
  // The decision comes before the items, which can be thousands of rows long.
  if (waiting) {
    draw_decision(detail, reachable, state);
  }
  draw_items(detail, waiting, state.selected[plan.id]);
  if (!waiting && tray_model::undoable(plan)) {
    ImGui::BeginDisabled(!reachable);
    if (ImGui::Button("Undo")) {
      state.requests.emplace_back(tray_model::UndoPlan{.id = plan.id});
    }
    ImGui::EndDisabled();
  }
}

void draw_plan(const PlanRow& plan, const CleanupView& cleanup, const std::chrono::time_zone& zone,
               bool reachable, UiState& state) {
  ImGui::PushID(std::to_string(plan.id).c_str());
  const bool waiting = tray_model::pending(plan);
  const std::string header =
      waiting
          ? std::format("{}  -  {}  -  {}  -  tier {}###plan", plan.title, size_text(plan.bytes),
                        plural(plan.items, "item"), plan.tier)
          : std::format("{}  -  {}  -  {}###plan", plan.title, plan.state, size_text(plan.bytes));
  ImGui::PushStyleColor(ImGuiCol_Text, ui::rgb(waiting ? color::text : state_color(plan.state)));
  // The owner's open plans are the state; a click toggles it.
  ImGui::SetNextItemOpen(state.open_plans.contains(plan.id));
  const bool open = ImGui::CollapsingHeader(header.c_str());
  ImGui::PopStyleColor();
  track_open(plan.id, open, state);
  if (open) {
    muted_text(std::format("{} plan, proposed by {} on {}, run by {}", plan.kind, plan.proposer,
                           local_date_time(zone, plan.created_at), runner(plan)));
    if (plan.needs_restart) {
      colored_text(color::warning, "Takes effect after a restart.");
    }
    if (const PlanDetail* detail = cleanup.detail(plan.id)) {
      draw_detail(*detail, reachable, state);
    } else {
      muted_text("Loading the items...");
    }
  }
  ImGui::PopID();
}

// "Delete now" of one quarantine run, with a confirmation; "Deleting..." while it runs.
void draw_purge(const tray_model::QuarantineRun& run, bool purging, bool reachable,
                UiState& state) {
  if (purging) {
    colored_text(color::accent, "Deleting " + size_text(run.bytes) + " permanently...");
    return;
  }
  ImGui::BeginDisabled(!reachable);
  if (state.confirm_purge != run.plan) {
    if (ImGui::Button(std::format("Delete now (frees {})", size_text(run.bytes)).c_str())) {
      state.confirm_purge = run.plan;
    }
  } else {
    wrapped_text(color::warning,
                 std::format("Delete these {} permanently? They can no longer be "
                             "restored.",
                             plural(static_cast<std::int64_t>(run.items.size()), "item")));
    if (ImGui::Button("Delete permanently")) {
      state.requests.emplace_back(tray_model::PurgeQuarantine{.plan = run.plan});
      state.confirm_purge.reset();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      state.confirm_purge.reset();
    }
  }
  ImGui::EndDisabled();
}

void draw_quarantine(const std::vector<tray_model::QuarantineRun>& runs,
                     const std::vector<std::int64_t>& purging, const std::chrono::time_zone& zone,
                     bool reachable, UiState& state) {
  if (runs.empty()) {
    muted_text("Nothing is in quarantine.");
    return;
  }
  muted_text(
      "Quarantined items are moved aside, not deleted, until their purge date. Delete now "
      "frees their space at once.");
  for (const tray_model::QuarantineRun& run : runs) {
    ImGui::PushID(std::to_string(run.plan).c_str());
    const std::string header =
        std::format("Plan {}  -  {}  -  {}  -  purged after {}###run", run.plan,
                    plural(static_cast<std::int64_t>(run.items.size()), "item"),
                    size_text(run.bytes), local_date_time(zone, run.purge_after));
    const bool open = ImGui::CollapsingHeader(header.c_str());
    draw_purge(run, std::ranges::find(purging, run.plan) != purging.end(), reachable, state);
    if (open) {
      muted_text("Moved " + local_date_time(zone, run.moved_at));
      ImGui::BeginDisabled(!reachable);
      if (ImGui::Button("Restore all")) {
        state.requests.emplace_back(tray_model::RestoreQuarantine{.plan = run.plan});
      }
      if (ImGui::BeginTable("items", 3, table_flags)) {
        ImGui::TableSetupColumn("Original location", ImGuiTableColumnFlags_WidthStretch, 5.0F);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.8F);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.8F);
        ImGui::TableHeadersRow();
        for (const tray_model::QuarantineItemRow& item : run.items) {
          ImGui::TableNextRow();
          ImGui::PushID(std::to_string(item.item).c_str());
          ImGui::TableNextColumn();
          wrapped_text(color::text, item.original);
          ImGui::TableNextColumn();
          text(size_text(item.bytes));
          ImGui::TableNextColumn();
          if (ImGui::SmallButton("Restore")) {
            state.requests.emplace_back(
                tray_model::RestoreQuarantine{.plan = run.plan, .item = item.item});
          }
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      ImGui::EndDisabled();
    }
    ImGui::PopID();
  }
}

// Space lists. A file row can be proposed for cleaning: the service creates a plan that waits
// in the inbox like any other.

void propose_button(std::vector<std::int64_t> candidates, const char* label, bool reachable,
                    UiState& state) {
  ImGui::BeginDisabled(!reachable);
  if (ImGui::SmallButton(label)) {
    state.requests.emplace_back(tray_model::ProposeCleanup{.candidates = std::move(candidates)});
  }
  ImGui::EndDisabled();
}

void draw_top(const tray_model::SpaceReply& reply, const std::chrono::time_zone& zone,
              bool reachable, UiState& state) {
  ImGui::SeparatorText("Largest folders");
  if (ImGui::BeginTable("folders", 3, table_flags)) {
    ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch, 5.0F);
    ImGui::TableSetupColumn("On disk", ImGuiTableColumnFlags_WidthStretch, 1.0F);
    ImGui::TableSetupColumn("Files", ImGuiTableColumnFlags_WidthStretch, 0.8F);
    ImGui::TableHeadersRow();
    for (const tray_model::TopFolderRow& folder : reply.top_folders) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      wrapped_text(color::text, folder.path);
      ImGui::TableNextColumn();
      text(size_text(folder.size_on_disk));
      ImGui::TableNextColumn();
      muted_text(std::to_string(folder.files));
    }
    ImGui::EndTable();
  }
  ImGui::SeparatorText("Largest files");
  if (ImGui::BeginTable("files", 4, table_flags)) {
    ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 5.0F);
    ImGui::TableSetupColumn("On disk", ImGuiTableColumnFlags_WidthStretch, 1.0F);
    ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthStretch, 1.4F);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.4F);
    ImGui::TableHeadersRow();
    for (const tray_model::TopFileRow& file : reply.top_files) {
      ImGui::TableNextRow();
      ImGui::PushID(std::to_string(file.id).c_str());
      ImGui::TableNextColumn();
      wrapped_text(color::text, file.path);
      ImGui::TableNextColumn();
      text(size_text(file.size_on_disk));
      ImGui::TableNextColumn();
      muted_text(local_date_time(zone, file.modified));
      ImGui::TableNextColumn();
      propose_button({file.id}, "Propose cleanup", reachable, state);
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
}

void draw_downloads(const std::vector<tray_model::OldDownloadRow>& downloads, bool reachable,
                    UiState& state) {
  if (!ImGui::BeginTable("downloads", 5, table_flags)) {
    return;
  }
  ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 3.0F);
  ImGui::TableSetupColumn("On disk", ImGuiTableColumnFlags_WidthStretch, 0.8F);
  ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthStretch, 0.7F);
  ImGui::TableSetupColumn("Downloaded from", ImGuiTableColumnFlags_WidthStretch, 3.0F);
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.2F);
  ImGui::TableHeadersRow();
  for (const tray_model::OldDownloadRow& download : downloads) {
    ImGui::TableNextRow();
    ImGui::PushID(std::to_string(download.id).c_str());
    ImGui::TableNextColumn();
    wrapped_text(color::text, download.path);
    ImGui::TableNextColumn();
    text(size_text(download.size_on_disk));
    ImGui::TableNextColumn();
    text(plural(download.age_days, "day"));
    ImGui::TableNextColumn();
    wrapped_text(color::muted, download.origin.value_or("unknown"));
    ImGui::TableNextColumn();
    propose_button({download.id}, "Propose cleanup", reachable, state);
    ImGui::PopID();
  }
  ImGui::EndTable();
}

void draw_duplicates(const std::vector<tray_model::DuplicateGroupRow>& groups,
                     const std::chrono::time_zone& zone, bool reachable, UiState& state) {
  for (std::size_t index = 0; index < groups.size(); ++index) {
    const tray_model::DuplicateGroupRow& group = groups[index];
    ImGui::PushID(static_cast<int>(index));
    const std::string header =
        std::format("{} copies of {}  -  {} reclaimable###group", group.files.size(),
                    size_text(group.size), size_text(group.reclaimable));
    if (ImGui::CollapsingHeader(header.c_str())) {
      const tray_model::DuplicateFileRow* keep = tray_model::keep_hint(group);
      std::vector<std::int64_t> others;
      for (const tray_model::DuplicateFileRow& file : group.files) {
        const bool kept = &file == keep;
        if (!kept) {
          others.push_back(file.id);
        }
        colored_text(kept ? color::good : color::text,
                     (kept ? "keep    " : "copy    ") + file.path + "  (modified " +
                         local_date_time(zone, file.modified) + ")");
      }
      muted_text("Keeping the oldest copy, most likely the original, frees the rest.");
      propose_button(std::move(others), "Propose removing the other copies", reachable, state);
    }
    ImGui::PopID();
  }
}

void draw_installers(const std::vector<tray_model::UsedInstallerRow>& installers, bool reachable,
                     UiState& state) {
  if (!ImGui::BeginTable("installers", 5, table_flags)) {
    return;
  }
  ImGui::TableSetupColumn("Installer", ImGuiTableColumnFlags_WidthStretch, 3.0F);
  ImGui::TableSetupColumn("On disk", ImGuiTableColumnFlags_WidthStretch, 0.8F);
  ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 0.5F);
  ImGui::TableSetupColumn("Installed as", ImGuiTableColumnFlags_WidthStretch, 2.0F);
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.2F);
  ImGui::TableHeadersRow();
  for (const tray_model::UsedInstallerRow& installer : installers) {
    ImGui::TableNextRow();
    ImGui::PushID(std::to_string(installer.id).c_str());
    ImGui::TableNextColumn();
    wrapped_text(color::text, installer.path);
    ImGui::TableNextColumn();
    text(size_text(installer.size_on_disk));
    ImGui::TableNextColumn();
    muted_text(installer.kind);
    ImGui::TableNextColumn();
    text(installer.installed_as);
    ImGui::TableNextColumn();
    propose_button({installer.id}, "Propose cleanup", reachable, state);
    ImGui::PopID();
  }
  ImGui::EndTable();
}

// The volume picker of the largest items; starts on the Windows volume.
void draw_volume_picker(const DashboardModel& model, UiState& state) {
  if (state.space_volume.empty()) {
    const auto system = std::ranges::find_if(model.volumes(), &tray_model::VolumeRow::system);
    state.space_volume = system != model.volumes().end() ? system->name : "C:";
  }
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0F);
  if (ImGui::BeginCombo("Volume", state.space_volume.c_str())) {
    for (const tray_model::VolumeRow& volume : model.volumes()) {
      if (volume.name.size() != 2 || volume.name[1] != ':') {
        continue;  // space.top takes a drive letter; a folder mount has none
      }
      if (ImGui::Selectable(volume.name.c_str(), volume.name == state.space_volume)) {
        state.space_volume = volume.name;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
}

// Loads the list shown once (the service's newest result, or a first analysis), and again on
// Refresh with a new analysis.
void request_space(bool reachable, UiState& state) {
  const std::string volume =
      state.space == tray_model::SpaceList::top ? state.space_volume : std::string();
  const std::string key = std::format("{}/{}", static_cast<int>(state.space), volume);
  ImGui::BeginDisabled(!reachable);
  const bool refresh = ImGui::Button("Refresh");
  ImGui::EndDisabled();
  if (refresh || (reachable && state.space_asked.insert(key).second)) {
    state.requests.emplace_back(
        tray_model::LoadSpace{.list = state.space, .volume = volume, .refresh = refresh});
  }
}

void draw_space_state(const tray_model::SpaceReply& reply, const std::chrono::time_zone& zone) {
  if (reply.state == "scanning") {
    colored_text(color::accent, "Analysing...");
  }
  if (!reply.error.empty()) {
    wrapped_text(color::bad, reply.error);
  }
  if (reply.scanned_at) {
    muted_text("As of " + local_date_time(zone, *reply.scanned_at));
  }
}

}  // namespace

void draw_cleanup(const DashboardModel& model, const std::chrono::time_zone& zone, UiState& state) {
  const CleanupView& cleanup = model.cleanup();
  const bool reachable = model.service_reachable();
  wrapped_text(color::accent,
               "Nothing in this inbox runs without your click: each plan waits until you approve "
               "it, and only the items you approve are acted on. Only rules of tier \"auto\" "
               "(regenerable caches) clean on their own schedule.");
  draw_scan(cleanup, zone, reachable, state);
  if (!cleanup.action_error().empty()) {
    wrapped_text(color::bad, cleanup.action_error());
  }
  draw_task(cleanup);
  const auto& plans = cleanup.plans();
  if (!plans) {
    muted_text("Plans: " + std::string(tray_model::not_available) +
               " (not reported by the service).");
    return;
  }
  std::vector<const PlanRow*> waiting;
  std::vector<const PlanRow*> others;
  for (const PlanRow& plan : *plans) {
    (tray_model::pending(plan) ? waiting : others).push_back(&plan);
  }
  ImGui::SeparatorText(std::format("Waiting for your approval ({})", waiting.size()).c_str());
  if (waiting.empty()) {
    muted_text("No plan is waiting.");
  } else {
    text("Reclaimable by tier: " + totals_text(tray_model::pending_bytes_by_tier(*plans)));
    text("By kind: " + totals_text(tray_model::pending_bytes_by_kind(*plans)));
    Bytes total{};
    for (const PlanRow* plan : waiting) {
      total += plan->bytes;
    }
    ImGui::BeginDisabled(!reachable);
    if (ImGui::Button(std::format("Approve every plan ({}, {})",
                                  plural(static_cast<std::int64_t>(waiting.size()), "plan"),
                                  size_text(total))
                          .c_str())) {
      for (const PlanRow* plan : waiting) {
        state.requests.emplace_back(
            tray_model::ApprovePlan{.id = plan->id, .selection = tray_model::select_all()});
        state.selected.erase(plan->id);
      }
    }
    ImGui::EndDisabled();
    for (const PlanRow* plan : waiting) {
      draw_plan(*plan, cleanup, zone, reachable, state);
    }
  }
  ImGui::SeparatorText("Recent plans");
  if (others.empty()) {
    muted_text("None.");
  }
  for (const PlanRow* plan : others) {
    draw_plan(*plan, cleanup, zone, reachable, state);
  }
  ImGui::SeparatorText("Quarantine");
  if (const auto& quarantine = cleanup.quarantine()) {
    const auto& status = cleanup.status();
    draw_quarantine(*quarantine, status ? status->purging : std::vector<std::int64_t>{}, zone,
                    reachable, state);
  } else {
    muted_text("Quarantine: " + std::string(tray_model::not_available));
  }
}

void draw_space(const DashboardModel& model, const std::chrono::time_zone& zone, UiState& state) {
  const bool reachable = model.service_reachable();
  wrapped_text(color::muted,
               "Read-only lists: nothing here changes a file. Propose cleanup creates a plan "
               "that waits in Cleanup until you approve it.");
  static constexpr std::array<std::pair<tray_model::SpaceList, const char*>, 4> lists = {{
      {tray_model::SpaceList::top, "Largest"},
      {tray_model::SpaceList::old_downloads, "Old downloads"},
      {tray_model::SpaceList::duplicates, "Duplicates"},
      {tray_model::SpaceList::used_installers, "Installers already used"},
  }};
  for (const auto& [list, label] : lists) {
    if (ImGui::RadioButton(label, state.space == list)) {
      state.space = list;
    }
    ImGui::SameLine();
  }
  ImGui::NewLine();
  if (state.space == tray_model::SpaceList::top) {
    draw_volume_picker(model, state);
  }
  request_space(reachable, state);
  if (!model.cleanup().action_error().empty()) {
    wrapped_text(color::bad, model.cleanup().action_error());
  }
  const auto& reply = model.cleanup().space(state.space);
  if (!reply) {
    muted_text("Loading...");
    return;
  }
  draw_space_state(*reply, zone);
  switch (state.space) {
    case tray_model::SpaceList::top:
      muted_text("Volume " + reply->volume);
      draw_top(*reply, zone, reachable, state);
      break;
    case tray_model::SpaceList::old_downloads:
      draw_downloads(reply->downloads, reachable, state);
      break;
    case tray_model::SpaceList::duplicates:
      draw_duplicates(reply->duplicates, zone, reachable, state);
      break;
    case tray_model::SpaceList::used_installers:
      draw_installers(reply->installers, reachable, state);
      break;
  }
}

}  // namespace myclean::tray
