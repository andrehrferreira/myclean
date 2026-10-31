#include "core/compress/compress_folder.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

#include "core/cleanup/support.hpp"
#include "core/compress/compression.hpp"
#include "core/model/action.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"
#include "core/space/text.hpp"

namespace myclean::compress {

namespace {

// Compresses one preview item while it is still the same plain, uncompressed file, and counts
// what happened in `report`. Returns the file's state before, when it was compressed.
std::optional<FileState> compress_item(IWof& wof, const PlanItem& item, Algorithm algorithm,
                                       CompressReport& report) {
  const Result<FileState> state = wof.state(item.path);
  if (!state) {
    if (state.error().code == Errc::not_found) {
      ++report.changed;
    } else {
      report.errors.push_back({.path = item.path, .error = state.error()});
    }
    return std::nullopt;
  }
  if (state->identity != item.identity || state->reparse_point || state->placeholder ||
      (!state->external && (state->ntfs_compressed || state->sparse))) {
    ++report.changed;
    return std::nullopt;
  }
  if (state->external) {
    ++report.already;
    return std::nullopt;
  }
  const Result<bool> compressed = wof.compress(item.path, algorithm, item.identity);
  if (!compressed) {
    switch (compressed.error().code) {
      case Errc::in_use:
        ++report.in_use;
        break;
      case Errc::not_found:
      case Errc::refused:
        ++report.changed;
        break;
      default:
        report.errors.push_back({.path = item.path, .error = compressed.error()});
        break;
    }
    return std::nullopt;
  }
  if (!*compressed) {
    ++report.not_beneficial;
    return std::nullopt;
  }
  ++report.compressed;
  return *state;
}

}  // namespace
CompressFolder::CompressFolder(const space::IDirectoryReader& reader, IWof& wof,
                               const os::IClock& clock, std::filesystem::path folder,
                               Algorithm algorithm, CompressOptions options, std::stop_token stop)
    : reader_(&reader),
      wof_(&wof),
      clock_(&clock),
      folder_(std::move(folder)),
      algorithm_(algorithm),
      options_(std::move(options)),
      stop_(std::move(stop)) {}

ActionInfo CompressFolder::describe() const {
  return {
      .what = std::format("Compress {} with WOF {}", space::utf8(folder_), to_string(algorithm_)),
      .why =
          "programs read the same content while it takes less space on disk; "
          "decompressing restores every file exactly",
      .risk = Risk::low,
      .tier = Tier::ask,
      .context = ExecutionContext::service};
}

Result<Preview> CompressFolder::preview() {
  if (!is_wof(algorithm_)) {
    return fail(Errc::invalid_argument, "LZNT1 is estimated for information only");
  }
  Result<space::ScanResult> scan = space::scan(*reader_, folder_, {}, stop_);
  if (!scan) {
    return tl::unexpected(scan.error());
  }
  if (scan->cancelled) {
    return fail(Errc::refused, "cancelled while listing " + space::utf8(folder_));
  }
  const WallTime now = clock_->now();
  Preview preview;
  std::int64_t next_id = 0;
  for (const space::Folder& folder : scan->folders) {
    for (const space::FileItem& file : folder.files) {
      if (exclusion(folder, file, options_.rules, now) == Exclusion::none) {
        preview.items.push_back(PlanItem{.id = PlanItemId{++next_id},
                                         .path = space::path_of(folder, file),
                                         .identity = file.identity,
                                         .size = file.size_on_disk});
      }
    }
  }
  return preview;
}

Result<Undo> CompressFolder::apply(const Preview& preview) {
  report_ = {};
  before_ = {};
  Undo undo;
  std::uint64_t done = 0;
  const MonoTime started = clock_->monotonic();
  const auto out_of_budget = [&] {
    return (options_.byte_budget.count() > 0 && done >= options_.byte_budget.count()) ||
           (options_.time_budget.count() > 0 &&
            clock_->monotonic() - started >= options_.time_budget);
  };
  for (const PlanItem& item : preview.items) {
    if (stop_.stop_requested() || out_of_budget()) {
      report_.stopped = true;
      break;
    }
    if (const std::optional<FileState> before = compress_item(*wof_, item, algorithm_, report_)) {
      done += before->size.count();
      before_ += before->size_on_disk;
      undo.items.push_back({.item = item.id, .data = space::utf8(item.path)});
    }
  }
  return undo;
}
Result<Verification> CompressFolder::verify(const Undo& undo) {
  Bytes after;
  for (const UndoItem& item : undo.items) {
    const Result<FileState> state = wof_->state(cleanup::from_utf8(item.data));
    if (!state) {
      return tl::unexpected(state.error());
    }
    after += state->size_on_disk;
  }
  return cleanup::compare("size_on_disk_bytes", before_, after);
}

Result<void> CompressFolder::undo(const Undo& undo) {
  std::uint64_t failed = 0;
  std::string first;
  for (const UndoItem& item : undo.items) {
    const std::filesystem::path path = cleanup::from_utf8(item.data);
    Result<FileState> state = wof_->state(path);
    if (state && !state->wof) {
      continue;  // decompressed already, or no longer WOF-compressed
    }
    if (!state && state.error().code == Errc::not_found) {
      continue;  // removed since: nothing to restore
    }
    const Result<void> restored = state ? wof_->decompress(path) : tl::unexpected(state.error());
    if (!restored) {
      if (failed++ == 0) {
        first = restored.error().message;
      }
    }
  }
  if (failed > 0) {
    return fail(Errc::io_error, std::format("{} of {} files could not be decompressed; first: {}",
                                            failed, undo.items.size(), first));
  }
  return {};
}

}  // namespace myclean::compress
