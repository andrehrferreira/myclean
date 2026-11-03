#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

// Answers of the cleaning, plan, quarantine, space and user-task methods in the shapes of
// core/ipc/plans.hpp.
namespace myclean::tray_model::testing {

using InboxJson = nlohmann::json;

inline constexpr std::int64_t shader_bytes = 3'328'599'654;  // 3.1 GB

// A PlanSummary of plans.list. Plan 7 is the approvals spec's "Pending plan shown": 12
// shader-cache items, 3.1 GB, tier ask, proposed by the scheduler.
inline InboxJson plan_summary_json(std::int64_t id, std::string_view state,
                                   std::string_view kind = "cleanup",
                                   std::string_view tier = "ask") {
  return InboxJson{{"id", id},
                   {"title", "Plan " + std::to_string(id)},
                   {"kind", kind},
                   {"tier", tier},
                   {"scope", kind == "native" || kind == "relocate" ? "user" : "service"},
                   {"state", state},
                   {"proposer", "scheduler"},
                   {"created_at", 1'000'000},
                   {"items", 12},
                   {"bytes", shader_bytes},
                   {"needs_restart", false}};
}

// plans.list: plan 7 waiting (cleanup, ask), plan 8 waiting (native, auto), plan 5 verified
// (relocate), plan 4 failed.
inline InboxJson plans_json() {
  InboxJson shader = plan_summary_json(7, "awaiting_approval");
  shader["title"] = "Shader caches";
  return InboxJson{
      {"plans",
       InboxJson::array({shader, plan_summary_json(8, "awaiting_approval", "native", "auto"),
                         plan_summary_json(5, "verified", "relocate"),
                         plan_summary_json(4, "failed", "handler")})}};
}

// plans.get of plan `id` with `items` items (ids 100, 101, ...), each shader_bytes / 12.
inline InboxJson plan_get_json(std::int64_t id, int items = 12,
                               std::string_view state = "awaiting_approval") {
  InboxJson plan = plan_summary_json(id, state);
  InboxJson list = InboxJson::array();
  for (int index = 0; index < items; ++index) {
    list.push_back(InboxJson{
        {"id", 100 + index},
        {"path", R"(C:\Users\owner\AppData\Local\NVIDIA\DXCache\)" + std::to_string(index)},
        {"bytes", shader_bytes / 12},
        {"rule", index == 0 ? InboxJson(nullptr) : InboxJson("gpu.nvidia_dx")},
        {"reason", "shader cache, regenerates"},
        {"approved", nullptr}});
  }
  plan["items"] = std::move(list);
  plan["items_total"] = items;
  plan["commands"] = InboxJson::array({"quarantine 12 items to C:\\.myclean-quarantine\\7"});
  plan["undo"] = "restore from quarantine within 7 days";
  plan["warnings"] = InboxJson::array({"games compile shaders again on their next start"});
  plan["params"] = InboxJson::object();
  plan["freed"] = nullptr;
  plan["detail"] = nullptr;
  return plan;
}

inline InboxJson cleaning_status_json(bool scanning, std::int64_t last_completed = 990'000) {
  return InboxJson{{"state", scanning ? "scanning" : "idle"},
                   {"progress", scanning ? 0.4 : 1.0},
                   {"last_completed", last_completed},
                   {"candidates", 42},
                   {"bytes", 5'000'000'000LL},
                   {"next_auto_run", 1'050'000},
                   {"purging", InboxJson::array({3})},
                   {"unavailable", nullptr}};
}

// quarantine.list: one run of plan 3 with two items.
inline InboxJson quarantine_json() {
  return InboxJson{
      {"runs",
       InboxJson::array({InboxJson{
           {"plan", 3},
           {"moved_at", 900'000},
           {"purge_after", 900'000 + (7 * 86'400)},
           {"bytes", 3000},
           {"items", InboxJson::array({InboxJson{{"item", 31},
                                                 {"original", R"(F:\app\node_modules)"},
                                                 {"location", R"(F:\.myclean-quarantine\3\31)"},
                                                 {"bytes", 2000}},
                                       InboxJson{{"item", 32},
                                                 {"original", R"(F:\app\dist)"},
                                                 {"location", R"(F:\.myclean-quarantine\3\32)"},
                                                 {"bytes", 1000}}})}}})}};
}

inline InboxJson space_state(std::string_view state) {
  return InboxJson{{"state", state}, {"scanned_at", 999'000}, {"error", nullptr}};
}

inline InboxJson space_top_json(std::string_view state = "ready") {
  InboxJson answer = space_state(state);
  answer["path"] = "C:\\";
  answer["files"] = InboxJson::array({InboxJson{{"id", 501},
                                                {"path", R"(C:\hiberfil.sys)"},
                                                {"size_on_disk", 54'653'000'000LL},
                                                {"size", 54'653'000'000LL},
                                                {"modified", 998'000},
                                                {"protected", "page_file"}}});
  answer["folders"] = InboxJson::array({InboxJson{{"path", R"(C:\Windows)"},
                                                  {"size_on_disk", 40'000'000'000LL},
                                                  {"size", 41'000'000'000LL},
                                                  {"files", 300'000}}});
  return answer;
}

inline InboxJson old_downloads_json() {
  InboxJson answer = space_state("ready");
  answer["folder"] = R"(C:\Users\owner\Downloads)";
  const auto file = [](std::int64_t id, std::string_view name, InboxJson origin) {
    return InboxJson{
        {"id", id},        {"path", R"(C:\Users\owner\Downloads\)" + std::string(name)},
        {"size", 1000},    {"size_on_disk", 4096},
        {"created", 100},  {"modified", 100},
        {"age_days", 120}, {"origin", std::move(origin)}};
  };
  answer["files"] = InboxJson::array({file(601, "setup.zip",
                                           {{"zone_id", 3},
                                            {"host_url", "https://example.com/setup.zip"},
                                            {"referrer_url", nullptr}}),
                                      file(602, "old.txt", nullptr)});
  return answer;
}

inline InboxJson duplicates_json() {
  InboxJson answer = space_state("ready");
  answer["roots"] = InboxJson::array({R"(C:\Users\owner\Pictures)"});
  answer["groups"] = InboxJson::array(
      {InboxJson{{"size", 5000},
                 {"hash", "ab12"},
                 {"reclaimable", 8192},
                 {"files", InboxJson::array({InboxJson{{"id", 701},
                                                       {"path", R"(D:\photos\a.jpg)"},
                                                       {"size_on_disk", 8192},
                                                       {"modified", 2000}},
                                             InboxJson{{"id", 702},
                                                       {"path", R"(K:\archive\a-copy.jpg)"},
                                                       {"size_on_disk", 8192},
                                                       {"modified", 1000}}})}}});
  answer["reclaimable"] = 8192;
  answer["errors"] = InboxJson::array();
  return answer;
}

inline InboxJson used_installers_json() {
  InboxJson answer = space_state("ready");
  answer["folder"] = R"(C:\Users\owner\Downloads)";
  answer["used"] = InboxJson::array({InboxJson{{"id", 801},
                                               {"path", R"(C:\Users\owner\Downloads\tool.msi)"},
                                               {"kind", "msi"},
                                               {"size", 30'000'000},
                                               {"size_on_disk", 30'000'256},
                                               {"installed_as", "Tool 1.2"},
                                               {"product_code", "{1234}"}}});
  answer["errors"] = InboxJson::array();
  return answer;
}

// space.owner_decisions: a cold folder with its archive folder, one in Documents (protected),
// one without an HDD with room, and a program in an uninstall area.
inline InboxJson owner_decisions_json() {
  InboxJson answer = space_state("ready");
  answer["profile"] = R"(C:\Users\owner)";
  answer["cold"] = InboxJson::array(
      {InboxJson{{"id", 901},
                 {"path", R"(C:\Users\owner\AppData\Local\OldStudio)"},
                 {"bytes", 12'000'000'000},
                 {"target", R"(L:\Archive\C\Users\owner\AppData\Local\OldStudio)"},
                 {"reason", "nothing inside changed since 2025-07-01; 11.2 GiB on nvme"}},
       InboxJson{{"id", 902},
                 {"path", R"(C:\Users\owner\Documents)"},
                 {"bytes", 3'000'000'000},
                 {"target", R"(L:\Archive\C\Users\owner\Documents)"},
                 {"reason", "nothing inside changed since 2025-01-01; 2.8 GiB on nvme"},
                 {"protected", "user_known_folder"}},
       InboxJson{{"id", 903},
                 {"path", R"(C:\Users\owner\Huge)"},
                 {"bytes", 9'000'000'000'000},
                 {"target", nullptr},
                 {"reason",
                  "nothing inside changed since 2024-01-01; 8.2 TiB on nvme; no healthy "
                  "HDD keeps 10 % free after taking it"}}});
  answer["uninstall"] = InboxJson::array(
      {InboxJson{{"id", 904},
                 {"program", "AmpliTube 5"},
                 {"publisher", "IK Multimedia"},
                 {"version", "5.10"},
                 {"location", R"(C:\Program Files\IK Multimedia\AmpliTube 5)"},
                 {"bytes", 6'600'000'000},
                 {"area", "Audio plug-ins"},
                 {"reason", "Audio plug-ins: uninstall the ones you no longer use"}}});
  answer["notes"] = InboxJson::array({"the drive of D:\\ is unknown"});
  return answer;
}

// The "task" of a user_tasks.next answer: plan 8 runs `pnpm store prune`.
inline InboxJson native_task_json() {
  return InboxJson{{"plan", 8},
                   {"title", "Run `pnpm store prune`"},
                   {"kind", "native"},
                   {"operation", "apply"},
                   {"tier", "auto"},
                   {"action", {{"command", "pnpm store prune"}}},
                   {"items", InboxJson::array({InboxJson{{"id", 81},
                                                         {"path", "native:pnpm store prune"},
                                                         {"bytes", 2000},
                                                         {"approved", true},
                                                         {"volume_serial", 1},
                                                         {"file_id", "00ff"}}})}};
}

}  // namespace myclean::tray_model::testing
