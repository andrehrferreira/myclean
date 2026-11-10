#include "core/vhdx/compact.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/action.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/text.hpp"
#include "core/os/file_system.hpp"
#include "core/vhdx/report.hpp"

namespace myclean::vhdx {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::string_view not_reversible =
    "compaction only gives unused space back; the disk keeps its data";

fs::path path_of(const std::string& text) {
  return {std::u8string(text.begin(), text.end())};
}

// The disk `file` as an item, or why it cannot be one.
Result<os::FileStat> disk_stat(const os::IFileSystem& file_system, const fs::path& file) {
  if (!is_vhdx(file)) {
    return fail(Errc::invalid_argument, "not a .vhdx file: " + utf8_text(file));
  }
  Result<os::FileStat> status = file_system.stat(file);
  if (!status) {
    return tl::unexpected(status.error());
  }
  if (status->kind != os::EntryKind::file || status->attributes.reparse_point ||
      status->attributes.cloud_placeholder()) {
    return fail(Errc::invalid_argument, "not a plain file: " + utf8_text(file));
  }
  return status;
}

}  // namespace

ActionInfo CompactVhdx::describe() const {
  return ActionInfo{.what = "Compact WSL and Docker virtual disks",
                    .why =
                        "A WSL or Docker Desktop disk grows as Linux writes and never shrinks "
                        "by itself; compacting gives the space freed inside it back to the "
                        "drive. Every running distribution and container stops first",
                    .risk = Risk::medium,
                    .tier = Tier::ask,
                    .context = ExecutionContext::service};
}

Result<Preview> CompactVhdx::preview() {
  Preview preview;
  for (const fs::path& file : files_) {
    if (std::ranges::count(files_, file) > 1) {
      return fail(Errc::invalid_argument, "disk named twice: " + utf8_text(file));
    }
    Result<os::FileStat> status = disk_stat(*file_system_, file);
    if (!status) {
      return tl::unexpected(status.error());
    }
    preview.items.push_back(
        PlanItem{.path = file, .identity = status->identity, .size = status->size});
  }
  return preview;
}

Result<Undo> CompactVhdx::apply(const Preview& preview) {
  std::vector<Bytes> before;
  for (const PlanItem& item : preview.items) {
    Result<os::FileStat> status = disk_stat(*file_system_, item.path);
    if (!status) {
      return tl::unexpected(status.error());
    }
    if (status->identity != item.identity) {
      return fail(Errc::refused, "another file is at " + utf8_text(item.path) + " now");
    }
    before.push_back(status->size);
  }
  if (Result<void> stopped = wsl_->shutdown(); !stopped) {
    return fail(stopped.error().code,
                "wsl --shutdown failed, nothing was compacted: " + stopped.error().message);
  }
  Undo undo{.items = {}, .not_reversible = std::string(not_reversible)};
  std::optional<Error> first_error;
  bool compacted = false;
  for (std::size_t index = 0; index < preview.items.size(); ++index) {
    const PlanItem& item = preview.items[index];
    Json data{{"path", utf8_text(item.path)}, {"before", before[index].count()}};
    if (Result<void> done = disks_->compact(item.path); !done) {
      data["error"] = done.error().message;
      first_error = first_error.value_or(done.error());
    } else {
      const Result<os::FileStat> after = file_system_->stat(item.path);
      data["after"] = after ? after->size.count() : before[index].count();
      compacted = true;
    }
    undo.items.push_back(UndoItem{.item = item.id, .data = data.dump()});
  }
  if (!compacted && first_error) {
    return tl::unexpected(*first_error);
  }
  return undo;
}

Result<Verification> CompactVhdx::verify(const Undo& undo) {
  std::uint64_t before = 0;
  std::uint64_t now = 0;
  try {
    for (const UndoItem& item : undo.items) {
      const Json data = Json::parse(item.data);
      if (data.contains("error")) {
        continue;
      }
      const Result<os::FileStat> status =
          file_system_->stat(path_of(data.at("path").get<std::string>()));
      if (!status) {
        return tl::unexpected(status.error());
      }
      before += data.at("before").get<std::uint64_t>();
      now += status->size.count();
    }
  } catch (const Json::exception& error) {
    return fail(Errc::invalid_data, std::string("compaction undo data: ") + error.what());
  }
  Effect effect = Effect::unchanged;
  if (now < before) {
    effect = Effect::better;
  } else if (now > before) {
    effect = Effect::worse;
  }
  return Verification{.metric = "vhdx_bytes",
                      .before = static_cast<double>(before),
                      .after = static_cast<double>(now),
                      .effect = effect};
}

Result<void> CompactVhdx::undo(const Undo& /*undo*/) {
  return fail(Errc::not_reversible, std::string(not_reversible));
}

}  // namespace myclean::vhdx
