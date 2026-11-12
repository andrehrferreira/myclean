#include "cli/space_command.hpp"

#include <array>
#include <cctype>
#include <cstdint>
#include <format>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "cli/cli.hpp"
#include "cli/command.hpp"
#include "core/ipc/plans.hpp"
#include "core/mcp/client.hpp"

namespace myclean::cli {
namespace {

using Json = nlohmann::json;
using render::bytes;
using render::fixed;
using render::list_of;
using render::number;
using render::text_of;
using render::utc;

// The title line of an analysis and what its state means for the result below it.
std::string head(const Json& reply, const std::string& title) {
  const bool analysed = reply.value("scanned_at", Json()).is_number();
  std::string out = title + (analysed ? ", analysed " + utc(reply, "scanned_at") : "") + "\n";
  const std::string state = text_of(reply, "state");
  if (state == "scanning") {
    out += analysed ? "  A new analysis is running: this is the previous result. Run the command "
                      "again in a moment.\n"
                    : "  The analysis is running: run the command again in a moment.\n";
  } else if (state == "failed") {
    out += "  The analysis failed: " + text_of(reply, "error") + "\n";
  }
  return out;
}

// "  [id]   1.5 GiB  path" for a file entry.
std::string file_line(const Json& file, std::string_view size_key = "size_on_disk") {
  return std::format("  [{}] {:>10}  {}", fixed(number(file, "id"), 0),
                     bytes(number(file, size_key)), text_of(file, "path"));
}

// "  [id]   1.5 GiB  path (12 files)" for a folder entry; the id opens it with --folder.
std::string folder_line(const Json& folder) {
  return std::format("  [{}] {:>10}  {} ({} files)\n", fixed(number(folder, "id"), 0),
                     bytes(number(folder, "size_on_disk")), text_of(folder, "path"),
                     fixed(number(folder, "files"), 0));
}

std::string none_if_empty(const Json& list) {
  return list.empty() ? "  none\n" : "";
}

// The files an analysis could not read.
std::string not_read(const Json& errors) {
  std::string out;
  for (const Json& error : errors) {
    out += "  not read: " + text_of(error, "path") + " (" +
           text_of(error.value("error", Json::object()), "message") + ")\n";
  }
  return out;
}

std::string notes(const Json& reply) {
  std::string out;
  for (const Json& note : list_of(reply, "notes")) {
    out += "Note: " + (note.is_string() ? note.get<std::string>() : dump(note)) + "\n";
  }
  return out;
}

std::string top_text(const Json& reply) {
  std::string out = head(reply, "Largest files and folders of " + text_of(reply, "path"));
  if (!reply.contains("files")) {
    return out;
  }
  out += "Files:\n" + none_if_empty(list_of(reply, "files"));
  for (const Json& file : list_of(reply, "files")) {
    out += file_line(file);
    if (file.contains("protected")) {
      out += "  (protected: " + text_of(file, "protected") + "; it cannot be proposed)";
    }
    out += "\n";
  }
  out += "Folders:\n" + none_if_empty(list_of(reply, "folders"));
  for (const Json& folder : list_of(reply, "folders")) {
    out += folder_line(folder);
  }
  if (reply.contains("children")) {
    out += std::format("Folders directly inside ({} on disk, {} in its own files):\n",
                       bytes(number(reply, "size_on_disk")),
                       bytes(number(reply, "own_size_on_disk"))) +
           none_if_empty(list_of(reply, "children"));
    for (const Json& child : list_of(reply, "children")) {
      out += folder_line(child);
    }
  }
  return out + "Open a folder with: myclean space top --folder <id>\n";
}

std::string old_downloads_text(const Json& reply) {
  std::string out = head(
      reply, "Downloads created and modified more than 90 days ago in " + text_of(reply, "folder"));
  if (!reply.contains("files")) {
    return out;
  }
  out += none_if_empty(list_of(reply, "files"));
  for (const Json& file : list_of(reply, "files")) {
    const Json origin = file.value("origin", Json());
    const bool from = origin.is_object() && origin.value("host_url", Json()).is_string();
    out += file_line(file) + std::format(", {} days old", fixed(number(file, "age_days"), 0)) +
           (from ? ", from " + text_of(origin, "host_url") : "") + "\n";
  }
  return out;
}

std::string forgotten_text(const Json& reply) {
  std::string out = head(
      reply, std::format("Large forgotten files (at least {} on disk, untouched for {} days)",
                         bytes(number(reply, "min_size")), fixed(number(reply, "idle_days"), 0)));
  if (!reply.contains("files")) {
    return out;
  }
  out += none_if_empty(list_of(reply, "files"));
  for (const Json& file : list_of(reply, "files")) {
    out += file_line(file) +
           std::format(", untouched for {} days\n", fixed(number(file, "idle_days"), 0));
  }
  return out;
}

std::string duplicates_text(const Json& reply) {
  const bool photos = text_of(reply, "mode") == "photos";
  std::string out = head(reply, photos ? "Near-duplicate photos" : "Duplicate files");
  if (!reply.contains("groups")) {
    return out;
  }
  out += std::format("{} groups, {} reclaimable\n", fixed(number(reply, "groups_total"), 0),
                     bytes(number(reply, "reclaimable")));
  for (const Json& group : list_of(reply, "groups")) {
    out += std::format("  {} reclaimable{}:\n", bytes(number(group, "reclaimable")),
                       photos ? std::format(", photos differ in at most {} bits",
                                            fixed(number(group, "distance"), 0))
                              : std::string{});
    for (const Json& file : list_of(group, "files")) {
      out += "  " + file_line(file);
      if (photos) {
        out += std::format("  {}x{}{}", fixed(number(file, "width"), 0),
                           fixed(number(file, "height"), 0),
                           file.value("keep", false) ? ", the one to keep" : "");
      }
      out += "\n";
    }
  }
  if (reply.value("truncated", false)) {
    out += "  ... more groups than one answer holds: the most reclaimable are shown\n";
  }
  return out + not_read(list_of(reply, photos ? "not_analysed" : "errors"));
}

std::string installers_text(const Json& reply) {
  std::string out =
      head(reply, "Installers in " + text_of(reply, "folder") + " that are installed already");
  if (!reply.contains("used")) {
    return out;
  }
  out += none_if_empty(list_of(reply, "used"));
  for (const Json& installer : list_of(reply, "used")) {
    out +=
        file_line(installer) + std::format(" ({}, installed as {})\n", text_of(installer, "kind"),
                                           text_of(installer, "installed_as"));
  }
  return out + not_read(list_of(reply, "errors"));
}

std::string owner_decisions_text(const Json& reply) {
  std::string out = head(reply, "What only you can decide");
  if (!reply.contains("cold")) {
    return out;
  }
  out += "Folders on an SSD nothing has written to for six months:\n" +
         none_if_empty(list_of(reply, "cold"));
  for (const Json& folder : list_of(reply, "cold")) {
    std::string where = folder.value("target", Json()).is_string()
                            ? "would be archived to " + text_of(folder, "target")
                            : "no healthy HDD keeps 10 % free after taking it";
    if (folder.contains("protected")) {
      where = "protected: " + text_of(folder, "protected");
    }
    out +=
        file_line(folder, "bytes") + "\n      " + where + "; " + text_of(folder, "reason") + "\n";
  }
  out += "Programs to review for uninstall:\n" + none_if_empty(list_of(reply, "uninstall"));
  for (const Json& program : list_of(reply, "uninstall")) {
    out += std::format("  [{}] {:>10}  {} {} ({}): {}\n", fixed(number(program, "id"), 0),
                       bytes(number(program, "bytes")), text_of(program, "program"),
                       text_of(program, "version"), text_of(program, "publisher"),
                       text_of(program, "reason"));
  }
  return out + notes(reply) + "The one-click action of each is in the tray.\n";
}

std::string dev_drive_moves(const Json& reply) {
  std::string out =
      std::format("To move to a Dev Drive ({} in all; recommended size {}):\n",
                  bytes(number(reply, "total_bytes")), bytes(number(reply, "recommended_bytes")));
  const Json repositories = list_of(reply, "repositories");
  const Json caches = list_of(reply, "caches");
  out += repositories.empty() && caches.empty() ? "  nothing\n" : "";
  for (const Json& repository : repositories) {
    out += std::format("  {:>10}  repository {}, changed {}\n", bytes(number(repository, "bytes")),
                       text_of(repository, "path"), utc(repository, "changed"));
  }
  for (const Json& cache : caches) {
    const std::string relocation = text_of(cache, "relocation");
    out += std::format("  {:>10}  {} {}{}\n", bytes(number(cache, "bytes")),
                       text_of(cache, "label"), text_of(cache, "path"),
                       relocation.empty() || relocation == "n/a"
                           ? std::string{}
                           : " (MyClean relocates it as " + relocation + ")");
  }
  return out;
}

std::string dev_drive_text(const Json& reply) {
  std::string out = head(reply, "Dev Drive advice");
  if (!reply.contains("supported")) {
    return out;
  }
  out += std::format("Windows build {}: a Dev Drive is {}\n", text_of(reply, "build"),
                     reply.value("supported", false) ? "supported" : "not supported");
  out += text_of(reply, "explanation") + "\n";
  out += "Dev Drives here:\n" + none_if_empty(list_of(reply, "dev_drives"));
  for (const Json& drive : list_of(reply, "dev_drives")) {
    out += std::format(
        "  {} {} ({}): {}\n", text_of(drive, "volume"), text_of(drive, "file_system"),
        drive.value("trusted", false) ? "trusted" : "not trusted", text_of(drive, "note"));
  }
  out += dev_drive_moves(reply);
  const Json host = reply.value("host", Json());
  out += host.is_object()
             ? std::format("Where a new one would go: {} ({}, {} free)\n", text_of(host, "volume"),
                           text_of(host, "drive_class"), bytes(number(host, "free")))
             : "Where a new one would go: no healthy SSD keeps 10 % free\n";
  int step = 0;
  for (const Json& text : list_of(reply, "steps")) {
    out += std::format("  {}. {}\n", ++step, text.is_string() ? text.get<std::string>() : "");
  }
  return out + notes(reply);
}

std::string vhdx_text(const Json& reply) {
  std::string out = head(reply, "WSL and Docker virtual disks");
  if (!reply.contains("disks")) {
    return out;
  }
  out += std::format("{} on disk in all\n", bytes(number(reply, "total")));
  out += none_if_empty(list_of(reply, "disks"));
  for (const Json& disk : list_of(reply, "disks")) {
    const Json distribution = disk.value("distribution", Json());
    out += file_line(disk) + "  " + text_of(disk, "label") +
           (distribution.is_string() ? " " + distribution.get<std::string>() : "") +
           (disk.value("in_use", false) ? ", in use" : "") + "\n";
  }
  return out + notes(reply);
}

std::string indexed_text(const Json& reply) {
  std::string out = head(reply, "Build and cache folders Windows Search indexes");
  if (!reply.contains("folders")) {
    return out;
  }
  out += none_if_empty(list_of(reply, "folders"));
  for (const Json& folder : list_of(reply, "folders")) {
    out +=
        std::format("  [{}] {}  [{}] {}\n", fixed(number(folder, "id"), 0), text_of(folder, "path"),
                    text_of(folder, "category"), text_of(folder, "label"));
  }
  return out;
}

// The analyses that take only refresh: subcommand, method, text.
struct Analysis {
  std::string_view name;
  std::string_view method;
  std::string (*render)(const Json&);
};

constexpr std::array<Analysis, 7> analyses{
    Analysis{.name = "old-downloads",
             .method = ipc::space_old_downloads_method,
             .render = &old_downloads_text},
    Analysis{.name = "forgotten-files",
             .method = ipc::space_forgotten_files_method,
             .render = &forgotten_text},
    Analysis{.name = "used-installers",
             .method = ipc::space_used_installers_method,
             .render = &installers_text},
    Analysis{.name = "owner-decisions",
             .method = ipc::space_owner_decisions_method,
             .render = &owner_decisions_text},
    Analysis{.name = "dev-drive", .method = ipc::space_dev_drive_method, .render = &dev_drive_text},
    Analysis{.name = "vhdx", .method = ipc::space_vhdx_method, .render = &vhdx_text},
    Analysis{.name = "indexed-folders",
             .method = ipc::space_indexed_folders_method,
             .render = &indexed_text}};

// space top <drive>|downloads|profile [--limit <n>]: "C:" is sent as the token "C".
// space top --folder <id> [--limit <n>]: a folder by the id an earlier answer gave it.
int top(const CommandLine& line, const mcp::Caller& call, std::ostream& out, std::ostream& err) {
  const Result<std::optional<std::int64_t>> limit = option<std::int64_t>(line, "limit");
  const Result<std::optional<std::int64_t>> folder = option<std::int64_t>(line, "folder");
  if (!limit || !folder) {
    err << "myclean: " << (!limit ? limit.error() : folder.error()).message << '\n';
    return exit_usage;
  }
  Json params{{"refresh", line.refresh}};
  if (*folder) {
    params["folder"] = **folder;
  } else {
    std::string where = line.words[2];
    if (where.size() == 2 && where[1] == ':' &&
        std::isalpha(static_cast<unsigned char>(where[0])) != 0) {
      where.pop_back();
    }
    params["path"] = where;
  }
  if (*limit) {
    params["n"] = **limit;
  }
  return show(call(ipc::space_top_method, params), line.json, out, err, &top_text);
}

}  // namespace

int space_command(const CommandLine& line, const mcp::Caller& call, std::ostream& out,
                  std::ostream& err) {
  const std::vector<std::string>& words = line.words;
  const bool by_folder = find_option(line, "folder") != nullptr;
  if (words.size() == (by_folder ? 2U : 3U) && words[1] == "top") {
    return top(line, call, out, err);
  }
  if (words.size() == 2 && words[1] == "duplicates") {
    Json params{{"refresh", line.refresh}};
    if (const std::string* mode = find_option(line, "mode"); mode != nullptr) {
      params["mode"] = *mode;
    }
    return show(call(ipc::space_duplicates_method, params), line.json, out, err, &duplicates_text);
  }
  for (const Analysis& analysis : analyses) {
    if (words.size() == 2 && words[1] == analysis.name) {
      return show(call(analysis.method, Json{{"refresh", line.refresh}}), line.json, out, err,
                  analysis.render);
    }
  }
  err << "myclean: space: top <drive>|downloads|profile|--folder <id> [--limit <n>] | "
         "old-downloads | "
         "forgotten-files | duplicates [--mode exact|photos] | used-installers | "
         "owner-decisions | dev-drive | vhdx | indexed-folders [--refresh]\n";
  return exit_usage;
}

}  // namespace myclean::cli
