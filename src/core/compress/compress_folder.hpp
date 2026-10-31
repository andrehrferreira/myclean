#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <vector>

#include "core/compress/compression.hpp"
#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/space/scanner.hpp"

namespace myclean::compress {

struct CompressOptions {
  // Which files: the analyzer's rules, with its measured `incompressible` extensions added.
  Rules rules;
  // One run stops after compressing this many bytes (logical size) or after this long; the next
  // run of the same folder goes on where it stopped. Zero: no limit.
  Bytes byte_budget;
  std::chrono::milliseconds time_budget{0};
};

// What the last apply() did, file by file.
struct CompressReport {
  std::uint64_t compressed = 0;
  std::uint64_t already = 0;         // WOF-compressed before: done by an earlier run
  std::uint64_t not_beneficial = 0;  // WOF found nothing to save
  std::uint64_t in_use = 0;          // another program had it open for writing
  std::uint64_t changed = 0;         // another file, a link or no longer there
  bool stopped = false;              // by `stop` or a budget, before the last item
  std::vector<space::ItemError> errors;
};

// Compresses the files of a folder with WOF (compression spec, "Reversible Folder
// Compression"): tier ask, never automatic; run by the service. Programs keep reading the same
// bytes, so the change needs no restart and is undone exactly by decompressing. Every reference
// must outlive the action.
class CompressFolder final : public Action {
 public:
  CompressFolder(const space::IDirectoryReader& reader, IWof& wof, const os::IClock& clock,
                 std::filesystem::path folder, Algorithm algorithm, CompressOptions options,
                 std::stop_token stop = {});

  [[nodiscard]] ActionInfo describe() const override;
  // Scans the folder: one item per file the rules allow (ids 1..n in scan order), with its
  // identity and size on disk. Fails with invalid_argument for LZNT1, which is never applied.
  [[nodiscard]] Result<Preview> preview() override;
  // Compresses each item that is still the same file and not compressed yet, until `stop` or a
  // budget. Undo lists the compressed files (data: the path); it is reversible.
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  // Metric "size_on_disk_bytes" of the compressed files: before compression against now.
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  // Decompresses every listed file that is still WOF-compressed, also after a restart. Fails
  // with io_error naming the first failure when any file could not be decompressed (the others
  // are decompressed all the same).
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

  [[nodiscard]] const CompressReport& report() const { return report_; }

 private:
  const space::IDirectoryReader* reader_;
  IWof* wof_;
  const os::IClock* clock_;
  std::filesystem::path folder_;
  Algorithm algorithm_;
  CompressOptions options_;
  std::stop_token stop_;
  CompressReport report_;
  Bytes before_;
};

}  // namespace myclean::compress
