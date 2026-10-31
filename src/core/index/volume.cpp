#include "core/index/volume.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/index/mft.hpp"
#include "core/model/result.hpp"

namespace myclean::index {

Result<std::vector<std::byte>> read_extents(const INtfsVolume& volume,
                                            std::span<const Extent> extents, std::uint64_t length) {
  const Result<VolumeGeometry> geometry = volume.geometry();
  if (!geometry) {
    return tl::unexpected(geometry.error());
  }
  std::vector<std::byte> bytes;
  for (const Extent& extent : extents) {
    if (bytes.size() >= length) {
      break;
    }
    std::vector<std::byte> part(extent.clusters * geometry->bytes_per_cluster);
    if (extent.lcn) {
      if (Result<void> read = volume.read(*extent.lcn * geometry->bytes_per_cluster, part); !read) {
        return tl::unexpected(read.error());
      }
    }
    bytes.insert(bytes.end(), part.begin(), part.end());
  }
  bytes.resize(std::min<std::uint64_t>(bytes.size(), length));
  return bytes;
}

}  // namespace myclean::index
