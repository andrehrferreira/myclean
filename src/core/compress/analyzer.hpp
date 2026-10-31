#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "core/compress/compression.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/space/scanner.hpp"

// The compression analyzer (compression spec, "Sampled Compression Estimate"): estimates what
// WOF compression of a folder from a scan would save, per algorithm, by compressing samples in
// memory. Read-only.
//
// Sampling: the eligible files of the folder are laid end to end (in scan order) and `samples`
// equally spaced positions are picked over all their bytes, each the start of one window of
// up to 64 KiB aligned within its file. A file is thereby sampled in proportion to its size, so
// the pooled ratio (compressed bytes / read bytes) is an unbiased estimate for the folder's
// bytes. The windows read stay within the byte budget.
//
// Extrapolation: every eligible file is estimated as its size times the pooled ratio of the
// algorithm, plus the WOF chunk table, rounded up to clusters, and never above its size on disk
// (WOF keeps a file uncompressed when compression does not pay).
//
// LZX has no public compressor. Its estimate is modelled from XPRESS Huffman on 32 KiB chunks:
// LZX saves `lzx_gain` times what that saves. The factor was measured against real WOF LZX on
// 50 MB samples of a Rust toolchain (1.122) and of Program Files (1.127).
namespace myclean::compress {

// LZX savings / XPRESS-Huffman-32K savings, measured (see above).
inline constexpr double lzx_gain = 1.12;
// The spread of that factor, added to the relative error of the LZX estimate.
inline constexpr double lzx_model_error = 0.05;

struct AnalyzerOptions {
  Rules rules;
  Bytes budget{std::uint64_t{64} << 20U};  // content read for samples, at most
  Bytes window{std::uint64_t{64} << 10U};  // one sample; a multiple of 64 KiB
  std::chrono::milliseconds time_budget{std::chrono::minutes(2)};  // sampling stops after it
  // An extension whose samples keep more than this share under XPRESS16K is incompressible
  // here (Oodle-packed game archives, encrypted data); it needs `min_extension_samples`.
  double incompressible_ratio = 0.9;
  std::size_t min_extension_samples = 2;
  // A folder where more than this share of the bytes was modified within `rules.recent` is
  // written too often to compress.
  double frequently_written_share = 0.25;
  // Recommend nothing when the best estimate saves less than this or less than
  // `min_savings_share` of the eligible bytes.
  Bytes min_savings{std::uint64_t{64} << 20U};
  double min_savings_share = 0.1;
};

struct AlgorithmEstimate {
  Algorithm algorithm = Algorithm::xpress4k;
  Bytes savings;      // estimated bytes freed
  Bytes savings_low;  // 95 % interval of `savings`
  Bytes savings_high;
  // 1 - the relative half width of that interval, in [0, 1]: 1 when every eligible byte was
  // read, 0 when the samples cannot bound the estimate.
  double confidence = 0.0;
};

struct ExclusionTotal {
  Exclusion reason = Exclusion::none;
  std::uint64_t files = 0;
  Bytes size_on_disk;
};

struct FolderEstimate {
  std::filesystem::path folder;
  Category category = Category::other;
  Bytes size_on_disk;  // the whole folder
  Bytes eligible;      // size on disk of the files that would be compressed
  std::uint64_t eligible_files = 0;
  std::uint64_t samples = 0;
  Bytes sampled;                             // content read
  std::vector<ExclusionTotal> excluded;      // by reason, in Exclusion order
  std::vector<std::string> incompressible;   // extensions measured incompressible
  std::vector<AlgorithmEstimate> estimates;  // xpress4k, xpress8k, xpress16k, lzx, lznt1
  std::optional<Algorithm> recommended;
  std::string reason;                    // why that algorithm, or why none
  std::vector<space::ItemError> errors;  // samples that could not be read
  bool cancelled = false;                // stopped by `stop`: estimated from fewer samples

  // The estimate of `algorithm`; nullptr when it is not listed.
  [[nodiscard]] const AlgorithmEstimate* find(Algorithm algorithm) const;
};

// Estimates the folder `scan.folders[folder]` and everything below it. Files are judged with
// `options.rules` at `clock.now()`; sampling stops at the byte budget, at the time budget
// (`clock.monotonic()`) or when `stop` is requested.
[[nodiscard]] FolderEstimate estimate(const space::ScanResult& scan, std::size_t folder,
                                      const ICompressionProbe& probe, const os::IClock& clock,
                                      const AnalyzerOptions& options = {},
                                      const std::stop_token& stop = {});

}  // namespace myclean::compress
