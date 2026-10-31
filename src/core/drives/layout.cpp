#include "core/drives/layout.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <span>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/space/scanner.hpp"

namespace myclean::drives {

namespace {

constexpr std::uint64_t gibibyte = 1ULL << 30U;
constexpr std::uint8_t all_used = 0xFF;
constexpr unsigned bits_per_byte = 8;

// Reads the runs of one file into `sample`; a file that cannot be read counts as an error.
void add_file(FragmentationSample& sample, const IVolumeLayout& layout,
              const std::filesystem::path& path, Bytes size) {
  const Result<std::vector<Extent>> extents = layout.extents(path);
  if (!extents) {
    ++sample.errors;
    return;
  }
  const std::uint64_t fragments = count_fragments(*extents);
  ++sample.files;
  sample.bytes += size;
  sample.fragments += fragments;
  if (fragmented(fragments, size)) {
    ++sample.fragmented_files;
    sample.fragmented_bytes += size;
  }
  if (!sample.worst || fragments > sample.worst->fragments) {
    sample.worst = SampledFile{.path = path, .size = size, .fragments = fragments};
  }
}
}  // namespace

std::uint64_t count_fragments(std::span<const Extent> extents) {
  std::uint64_t fragments = 0;
  std::int64_t next = -1;  // the cluster that would continue the previous run
  for (const Extent& extent : extents) {
    if (extent.lcn < 0 || extent.clusters == 0) {
      continue;
    }
    if (extent.lcn != next) {
      ++fragments;
    }
    next = extent.lcn + static_cast<std::int64_t>(extent.clusters);
  }
  return fragments;
}

bool fragmented(std::uint64_t fragments, Bytes size) {
  return fragments > fragment_allowance && fragments * gibibyte > fragment_allowance * size.count();
}

void FreeSpaceTally::add(std::span<const std::uint8_t> bitmap, std::uint64_t clusters) {
  std::uint64_t cluster = 0;
  for (const std::uint8_t byte : bitmap) {
    if (cluster >= clusters) {
      break;
    }
    const std::uint64_t count = std::min<std::uint64_t>(bits_per_byte, clusters - cluster);
    if (byte == 0) {
      run_ += count;
    } else if (byte == all_used && count == bits_per_byte) {
      end_run(run_);
      run_ = 0;
    } else {
      for (unsigned bit = 0; bit < count; ++bit) {
        if (((byte >> bit) & 1U) == 0) {
          ++run_;
        } else {
          end_run(run_);
          run_ = 0;
        }
      }
    }
    cluster += count;
  }
}

void FreeSpaceTally::end_run(std::uint64_t run) {
  if (run == 0) {
    return;
  }
  free_ += run;
  ++runs_;
  largest_ = std::max(largest_, run);
  if (run * cluster_size_.count() < small_free_run.count()) {
    small_ += run;
  }
}

FreeSpaceLayout FreeSpaceTally::finish(bool complete) const {
  FreeSpaceTally closed = *this;
  closed.end_run(closed.run_);
  const std::uint64_t cluster = cluster_size_.count();
  return FreeSpaceLayout{.cluster_size = cluster_size_,
                         .free = Bytes{closed.free_ * cluster},
                         .free_runs = closed.runs_,
                         .largest_free_run = Bytes{closed.largest_ * cluster},
                         .free_in_small_runs = Bytes{closed.small_ * cluster},
                         .complete = complete};
}

double FragmentationSample::fragmented_percent() const {
  return bytes.count() == 0 ? 0.0
                            : 100.0 * static_cast<double>(fragmented_bytes.count()) /
                                  static_cast<double>(bytes.count());
}

double FragmentationSample::fragments_per_file() const {
  return files == 0 ? 0.0 : static_cast<double>(fragments) / static_cast<double>(files);
}

FragmentationSample sample_fragmentation(const std::filesystem::path& start,
                                         const space::IDirectoryReader& reader,
                                         const IVolumeLayout& layout, const os::IClock& clock,
                                         const SampleBudget& budget) {
  FragmentationSample sample;
  const MonoTime deadline = clock.monotonic() + budget.max_time;
  std::deque<std::filesystem::path> pending{start};
  while (!pending.empty()) {
    if (sample.folders >= budget.max_folders || sample.files >= budget.max_files ||
        clock.monotonic() >= deadline) {
      sample.stopped = true;
      break;
    }
    const std::filesystem::path folder = std::move(pending.front());
    pending.pop_front();
    Result<std::vector<space::ListedEntry>> entries = reader.list(folder);
    ++sample.folders;
    if (!entries) {
      ++sample.errors;
      continue;
    }
    for (const space::ListedEntry& entry : *entries) {
      if (entry.attributes.reparse_point || entry.attributes.cloud_placeholder()) {
        continue;
      }
      if (entry.directory) {
        pending.push_back(folder / entry.name);
        continue;
      }
      if (entry.size_on_disk < budget.min_file_size || sample.files >= budget.max_files ||
          clock.monotonic() >= deadline) {
        continue;
      }
      add_file(sample, layout, folder / entry.name, entry.size_on_disk);
    }
  }
  return sample;
}

}  // namespace myclean::drives
