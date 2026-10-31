#include "core/compress/analyzer.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <set>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "core/compress/compression.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/space/scanner.hpp"
#include "core/space/text.hpp"

namespace myclean::compress {

namespace {

constexpr std::size_t exclusion_count = static_cast<std::size_t>(Exclusion::incompressible) + 1;
constexpr std::array<Algorithm, 5> algorithms = {Algorithm::xpress4k, Algorithm::xpress8k,
                                                 Algorithm::xpress16k, Algorithm::lzx,
                                                 Algorithm::lznt1};
constexpr double z95 = 1.96;
constexpr std::uint64_t four_gib = std::uint64_t{4} << 30U;

struct Candidate {
  const space::Folder* folder = nullptr;
  const space::FileItem* file = nullptr;
  std::string extension;
};

struct Window {
  std::size_t candidate = 0;
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
};

struct Sample {
  std::size_t candidate = 0;
  SampleSizes sizes;
};

// The folder and every folder below it.
std::vector<std::size_t> subtree(const space::ScanResult& scan, std::size_t root) {
  std::vector<std::size_t> folders{root};
  for (std::size_t next = 0; next < folders.size(); ++next) {
    const std::vector<std::size_t>& children = scan.folders.at(folders[next]).folders;
    folders.insert(folders.end(), children.begin(), children.end());
  }
  return folders;
}

// The measured compressed bytes behind `algorithm`: for LZX its XPRESS-32K base.
std::uint64_t measured(const SampleSizes& sizes, Algorithm algorithm) {
  switch (algorithm) {
    case Algorithm::xpress4k:
      return sizes.xpress4k.count();
    case Algorithm::xpress8k:
      return sizes.xpress8k.count();
    case Algorithm::xpress16k:
      return sizes.xpress16k.count();
    case Algorithm::lzx:
      return sizes.xpress32k.count();
    case Algorithm::lznt1:
      break;
  }
  return sizes.lznt1.count();
}

std::uint64_t round_up(std::uint64_t value, std::uint64_t unit) {
  return unit == 0 ? value : (value + unit - 1) / unit * unit;
}

// Size on disk of `file` once compressed at `ratio` (compressed / original).
std::uint64_t after_compression(const space::FileItem& file, Algorithm algorithm, double ratio,
                                std::uint64_t cluster) {
  const std::uint64_t on_disk = file.size_on_disk.count();
  if (ratio >= 1.0) {
    return on_disk;
  }
  const std::uint64_t size = file.size.count();
  auto data = static_cast<std::uint64_t>(std::ceil(static_cast<double>(size) * ratio));
  if (is_wof(algorithm)) {
    // WOF stores one offset per chunk after the first: 4 bytes, 8 for files over 4 GiB.
    const std::uint64_t chunks = (size + chunk_bytes(algorithm) - 1) / chunk_bytes(algorithm);
    data += (chunks - 1) * (size > four_gib ? 8 : 4);
  }
  return std::min(on_disk, round_up(data, cluster));
}

// Window indices in bit-reversed order, so that sampling stopped early still covers the folder
// evenly instead of its first part only.
std::vector<std::size_t> spread_order(std::size_t count) {
  std::vector<std::size_t> order(count);
  for (std::size_t index = 0; index < count; ++index) {
    order[index] = index;
  }
  const int bits = count == 0 ? 0 : std::bit_width(count - 1);
  const auto reversed = [bits](std::size_t value) {
    std::size_t result = 0;
    for (int bit = 0; bit < bits; ++bit) {
      result = (result << 1U) | ((value >> static_cast<unsigned>(bit)) & 1U);
    }
    return result;
  };
  std::ranges::sort(order, {}, reversed);
  return order;
}

// The windows to read: every window of every file when the budget covers all bytes, otherwise
// `budget / window` equally spaced positions over the files laid end to end.
std::vector<Window> plan_windows(const std::vector<Candidate>& candidates, std::uint64_t total,
                                 const AnalyzerOptions& options) {
  const std::uint64_t window = std::max<std::uint64_t>(options.window.count(), 1);
  std::vector<Window> windows;
  if (total <= options.budget.count()) {
    for (std::size_t index = 0; index < candidates.size(); ++index) {
      const std::uint64_t size = candidates[index].file->size.count();
      for (std::uint64_t offset = 0; offset < size; offset += window) {
        windows.push_back(
            {.candidate = index, .offset = offset, .length = std::min(window, size - offset)});
      }
    }
    return windows;
  }
  const std::uint64_t count = std::max<std::uint64_t>(options.budget.count() / window, 1);
  std::vector<std::uint64_t> starts;  // of each candidate in the laid-out bytes
  starts.reserve(candidates.size());
  std::uint64_t start = 0;
  for (const Candidate& candidate : candidates) {
    starts.push_back(start);
    start += candidate.file->size.count();
  }
  for (std::uint64_t k = 0; k < count; ++k) {
    const std::uint64_t position = ((2 * k) + 1) * (total / (2 * count));
    const auto found = std::ranges::upper_bound(starts, position);
    const auto index = static_cast<std::size_t>(found - starts.begin()) - 1;
    const std::uint64_t size = candidates[index].file->size.count();
    const std::uint64_t offset = (position - starts[index]) / window * window;
    const Window next{
        .candidate = index, .offset = offset, .length = std::min(window, size - offset)};
    if (windows.empty() || windows.back().candidate != index || windows.back().offset != offset) {
      windows.push_back(next);
    }
  }
  return windows;
}

std::string gib(Bytes bytes) {
  constexpr double unit = 1024.0 * 1024.0 * 1024.0;
  return std::format("{:.1f} GiB", static_cast<double>(bytes.count()) / unit);
}

// The algorithm recommended for `category`, and why.
std::pair<Algorithm, std::string> preferred(Category category) {
  switch (category) {
    case Category::toolchain:
    case Category::engine:
    case Category::program:
      return {Algorithm::lzx,
              std::format("LZX: a {} install is read far more than written", to_string(category))};
    case Category::game:
      return {Algorithm::xpress8k,
              "XPRESS8K: games read large assets while playing; XPRESS "
              "decompresses fastest (LZX optional)"};
    case Category::other:
      break;
  }
  return {Algorithm::xpress4k, "XPRESS4K: the lightest WOF algorithm for content of unknown use"};
}

void recommend(FolderEstimate& result, Bytes recent_bytes, const AnalyzerOptions& options) {
  if (result.eligible_files == 0) {
    result.reason = "nothing to compress";
    return;
  }
  const double recent_share = static_cast<double>(recent_bytes.count()) /
                              static_cast<double>(recent_bytes.count() + result.eligible.count());
  if (recent_share > options.frequently_written_share) {
    result.reason =
        std::format("frequently written: {:.0f} % of its bytes changed in the last {} days",
                    recent_share * 100.0, options.rules.recent.count());
    return;
  }
  auto [algorithm, why] = preferred(result.category);
  const AlgorithmEstimate* chosen = result.find(algorithm);
  const auto threshold =
      std::max(options.min_savings.count(),
               static_cast<std::uint64_t>(static_cast<double>(result.eligible.count()) *
                                          options.min_savings_share));
  if (chosen == nullptr || chosen->savings.count() < threshold) {
    result.reason = std::format("estimated savings too small (under {})", gib(Bytes{threshold}));
    return;
  }
  result.recommended = algorithm;
  result.reason = std::format("{}; saves about {}", why, gib(chosen->savings));
  if (algorithm != Algorithm::lzx) {
    if (const AlgorithmEstimate* lzx = result.find(Algorithm::lzx); lzx != nullptr) {
      result.reason += std::format(" (LZX about {})", gib(lzx->savings));
    }
  }
}

AlgorithmEstimate estimate_algorithm(Algorithm algorithm, const std::vector<Sample>& samples,
                                     const std::vector<Candidate>& candidates,
                                     const std::vector<bool>& kept, std::uint64_t total,
                                     const FolderEstimate& result, const Rules& rules) {
  AlgorithmEstimate estimate{.algorithm = algorithm};
  double read = 0.0;
  double compressed = 0.0;
  for (const Sample& sample : samples) {
    read += static_cast<double>(sample.sizes.read.count());
    compressed += static_cast<double>(measured(sample.sizes, algorithm));
  }
  if (samples.empty() || read == 0.0) {
    estimate.savings_high = result.eligible;
    return estimate;
  }
  const double base = compressed / read;
  const double ratio =
      algorithm == Algorithm::lzx ? std::max(0.0, 1.0 - (lzx_gain * (1.0 - base))) : base;

  // Standard error of the ratio estimator, with the finite population correction.
  const auto n = static_cast<double>(samples.size());
  double residuals = 0.0;
  for (const Sample& sample : samples) {
    const double error = static_cast<double>(measured(sample.sizes, algorithm)) -
                         (base * static_cast<double>(sample.sizes.read.count()));
    residuals += error * error;
  }
  const double population = std::max(0.0, 1.0 - (read / static_cast<double>(total)));
  const double mean_read = read / n;
  // Relative half width of the 95 % interval; 1 (unbounded) when the samples cannot tell.
  double error = population > 0.0 ? 1.0 : 0.0;
  if (samples.size() >= 2) {
    const double standard = std::sqrt(residuals * population / (n * (n - 1.0))) / mean_read;
    const double saved = 1.0 - base;
    if (saved > 0.0) {
      error = z95 * standard / saved;
    } else if (standard == 0.0) {
      error = 0.0;
    }
  }
  if (algorithm == Algorithm::lzx) {
    error += lzx_model_error;
  }

  std::uint64_t savings = 0;
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    if (kept[index]) {
      const space::FileItem& file = *candidates[index].file;
      savings += file.size_on_disk.count() -
                 after_compression(file, algorithm, ratio, rules.cluster.count());
    }
  }
  const auto bound = [&](double factor) {
    return Bytes{
        std::min(result.eligible.count(),
                 static_cast<std::uint64_t>(std::max(0.0, static_cast<double>(savings) * factor)))};
  };
  estimate.savings = Bytes{savings};
  estimate.savings_low = bound(1.0 - error);
  estimate.savings_high = bound(1.0 + error);
  estimate.confidence = std::clamp(1.0 - error, 0.0, 1.0);
  return estimate;
}

using Exclusions = std::array<ExclusionTotal, exclusion_count>;

// The files of the folder's subtree the rules allow; every other file is added to `excluded`.
std::vector<Candidate> collect(const space::ScanResult& scan, std::size_t folder,
                               const Rules& rules, WallTime now, Exclusions& excluded) {
  std::vector<Candidate> candidates;
  for (const std::size_t index : subtree(scan, folder)) {
    const space::Folder& current = scan.folders.at(index);
    for (const space::FileItem& file : current.files) {
      const Exclusion reason = exclusion(current, file, rules, now);
      if (reason == Exclusion::none) {
        candidates.push_back(
            {.folder = &current, .file = &file, .extension = space::lower_extension(file.name)});
        continue;
      }
      ExclusionTotal& total = excluded.at(static_cast<std::size_t>(reason));
      ++total.files;
      if (!file.extra_link) {
        total.size_on_disk += file.size_on_disk;
      }
    }
  }
  return candidates;
}

// Reads the windows in spread order until the time budget or `stop`.
std::vector<Sample> take_samples(const std::vector<Candidate>& candidates,
                                 const std::vector<Window>& windows, const ICompressionProbe& probe,
                                 const os::IClock& clock, const AnalyzerOptions& options,
                                 const std::stop_token& stop, FolderEstimate& result) {
  std::vector<Sample> samples;
  const MonoTime started = clock.monotonic();
  for (const std::size_t index : spread_order(windows.size())) {
    if (stop.stop_requested()) {
      result.cancelled = true;
      break;
    }
    if (clock.monotonic() - started >= options.time_budget) {
      break;
    }
    const Window& window = windows[index];
    const std::filesystem::path path =
        space::path_of(*candidates[window.candidate].folder, *candidates[window.candidate].file);
    Result<SampleSizes> sizes = probe.sample(path, window.offset, window.length);
    if (!sizes) {
      result.errors.push_back({.path = path, .error = std::move(sizes.error())});
      continue;
    }
    result.sampled += sizes->read;
    samples.push_back({.candidate = window.candidate, .sizes = *sizes});
  }
  result.samples = samples.size();
  return samples;
}

// The extensions whose samples keep more than the incompressible ratio under XPRESS16K.
std::set<std::string> incompressible_extensions(const std::vector<Sample>& samples,
                                                const std::vector<Candidate>& candidates,
                                                const AnalyzerOptions& options) {
  struct Measured {
    std::size_t count = 0;
    double read = 0.0;
    double kept = 0.0;
  };
  std::map<std::string, Measured> by_extension;
  for (const Sample& sample : samples) {
    Measured& measured = by_extension[candidates[sample.candidate].extension];
    ++measured.count;
    measured.read += static_cast<double>(sample.sizes.read.count());
    measured.kept += static_cast<double>(sample.sizes.xpress16k.count());
  }
  std::set<std::string> incompressible;
  for (const auto& [extension, measured] : by_extension) {
    if (measured.count >= options.min_extension_samples && measured.read > 0.0 &&
        measured.kept / measured.read > options.incompressible_ratio) {
      incompressible.insert(extension);
    }
  }
  return incompressible;
}

}  // namespace

const AlgorithmEstimate* FolderEstimate::find(Algorithm algorithm) const {
  const auto found = std::ranges::find(estimates, algorithm, &AlgorithmEstimate::algorithm);
  return found == estimates.end() ? nullptr : &*found;
}

FolderEstimate estimate(const space::ScanResult& scan, std::size_t folder,
                        const ICompressionProbe& probe, const os::IClock& clock,
                        const AnalyzerOptions& options, const std::stop_token& stop) {
  const space::Folder& root = scan.folders.at(folder);
  FolderEstimate result{
      .folder = root.path, .category = categorize(root.path), .size_on_disk = root.size_on_disk};
  Exclusions excluded{};
  const std::vector<Candidate> candidates =
      collect(scan, folder, options.rules, clock.now(), excluded);
  std::uint64_t total = 0;
  for (const Candidate& candidate : candidates) {
    total += candidate.file->size.count();
  }
  std::vector<Sample> samples = take_samples(candidates, plan_windows(candidates, total, options),
                                             probe, clock, options, stop, result);

  // Extensions measured incompressible leave the estimate with their samples.
  const std::set<std::string> incompressible =
      incompressible_extensions(samples, candidates, options);
  result.incompressible.assign(incompressible.begin(), incompressible.end());
  std::vector<bool> kept(candidates.size(), true);
  std::uint64_t kept_total = 0;
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    const space::FileItem& file = *candidates[index].file;
    if (incompressible.contains(candidates[index].extension)) {
      kept[index] = false;
      ExclusionTotal& sum = excluded.at(static_cast<std::size_t>(Exclusion::incompressible));
      ++sum.files;
      sum.size_on_disk += file.size_on_disk;
      continue;
    }
    ++result.eligible_files;
    result.eligible += file.size_on_disk;
    kept_total += file.size.count();
  }
  std::erase_if(samples, [&](const Sample& sample) { return !kept[sample.candidate]; });

  for (const Algorithm algorithm : algorithms) {
    result.estimates.push_back(estimate_algorithm(algorithm, samples, candidates, kept,
                                                  std::max<std::uint64_t>(kept_total, 1), result,
                                                  options.rules));
  }
  for (std::size_t index = 0; index < exclusion_count; ++index) {
    if (excluded.at(index).files > 0) {
      excluded.at(index).reason = static_cast<Exclusion>(index);
      result.excluded.push_back(excluded.at(index));
    }
  }
  const Bytes recent = excluded.at(static_cast<std::size_t>(Exclusion::recent)).size_on_disk;
  recommend(result, recent, options);
  return result;
}

}  // namespace myclean::compress