#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/space/scanner.hpp"

// How files and free space lie on a volume: fragments per file from the file's cluster runs, a
// bounded sample of a volume's large files, and the free-space layout from the volume bitmap.
// Read-only throughout.
namespace myclean::drives {

// One run of a file's clusters, as FSCTL_GET_RETRIEVAL_POINTERS reports it.
struct Extent {
  std::int64_t lcn = 0;  // first logical cluster on the volume; -1 for a run without clusters
  std::uint64_t clusters = 0;
};

// The file's fragments: runs with clusters that do not continue the previous run with clusters.
// 0 when no run has clusters (a resident or sparse file).
[[nodiscard]] std::uint64_t count_fragments(std::span<const Extent> extents);

// A file counts as fragmented with more than this many fragments, and more than this many per
// GiB, so a large file written in a few big pieces does not.
inline constexpr std::uint64_t fragment_allowance = 8;

[[nodiscard]] bool fragmented(std::uint64_t fragments, Bytes size);

// Free runs smaller than this count as small: a large file written there is split.
inline constexpr Bytes small_free_run{64ULL << 20U};

struct FreeSpaceLayout {
  Bytes cluster_size;
  Bytes free;
  std::uint64_t free_runs = 0;  // maximal runs of free clusters
  Bytes largest_free_run;
  Bytes free_in_small_runs;  // free space in runs smaller than small_free_run
  bool complete = false;     // false when the read stopped early; the figures cover what was read
};

// Folds the volume bitmap (bit set = cluster in use, least significant bit first), read in
// pieces from cluster 0 on, into a FreeSpaceLayout.
class FreeSpaceTally {
 public:
  explicit FreeSpaceTally(Bytes cluster_size) : cluster_size_(cluster_size) {}

  // Adds the next `clusters` bits of the bitmap; `bitmap` holds at least (clusters + 7) / 8 bytes.
  void add(std::span<const std::uint8_t> bitmap, std::uint64_t clusters);
  [[nodiscard]] FreeSpaceLayout finish(bool complete) const;

 private:
  void end_run(std::uint64_t run);

  Bytes cluster_size_;
  std::uint64_t run_ = 0;  // free clusters in the run still open
  std::uint64_t free_ = 0;
  std::uint64_t runs_ = 0;
  std::uint64_t largest_ = 0;
  std::uint64_t small_ = 0;
};

// Reads the layout of files and free space. Implemented by src/platform/defrag.
class IVolumeLayout {
 public:
  virtual ~IVolumeLayout() = default;
  IVolumeLayout(const IVolumeLayout&) = delete;
  IVolumeLayout& operator=(const IVolumeLayout&) = delete;
  IVolumeLayout(IVolumeLayout&&) = delete;
  IVolumeLayout& operator=(IVolumeLayout&&) = delete;

  // The cluster runs of file `file`, in file order. Never opens the content for reading.
  [[nodiscard]] virtual Result<std::vector<Extent>> extents(
      const std::filesystem::path& file) const = 0;
  // The free-space layout of the volume at `root` from its bitmap, read for at most `budget`.
  // Fails with access_denied when the volume cannot be opened (it needs administrator rights).
  [[nodiscard]] virtual Result<FreeSpaceLayout> free_space(
      const std::filesystem::path& root, std::chrono::milliseconds budget) const = 0;

 protected:
  IVolumeLayout() = default;
};

// Limits of one fragmentation sample. Whichever runs out first stops the walk.
struct SampleBudget {
  Bytes min_file_size{64ULL << 20U};  // smaller files are not sampled
  std::uint64_t max_folders = 20'000;
  std::uint64_t max_files = 2'000;
  std::chrono::milliseconds max_time{std::chrono::seconds{30}};
};

struct SampledFile {
  std::filesystem::path path;
  Bytes size;  // size on disk
  std::uint64_t fragments = 0;
};

struct FragmentationSample {
  std::uint64_t folders = 0;  // folders listed
  std::uint64_t files = 0;    // files whose runs were read
  Bytes bytes;                // their size on disk
  std::uint64_t fragments = 0;
  std::uint64_t fragmented_files = 0;  // fragmented() ones
  Bytes fragmented_bytes;
  std::optional<SampledFile> worst;  // the file with the most fragments
  std::uint64_t errors = 0;          // folders or files that could not be read
  bool stopped = false;              // a budget ran out before the walk ended

  // Share of the sampled bytes in fragmented files, 0-100; 0 for an empty sample.
  [[nodiscard]] double fragmented_percent() const;
  [[nodiscard]] double fragments_per_file() const;
};

// Walks the tree under `start` breadth-first (links and cloud placeholders are skipped, never
// followed or opened) and reads the cluster runs of every file of at least
// budget.min_file_size, until the walk ends or the budget runs out. Folders and files that
// cannot be read are counted and skipped. The caller sets the thread's priority (background).
[[nodiscard]] FragmentationSample sample_fragmentation(const std::filesystem::path& start,
                                                       const space::IDirectoryReader& reader,
                                                       const IVolumeLayout& layout,
                                                       const os::IClock& clock,
                                                       const SampleBudget& budget);

}  // namespace myclean::drives
