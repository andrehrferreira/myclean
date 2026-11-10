#include "core/space/photos.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/identity_set.hpp"
#include "core/space/scanner.hpp"
#include "core/space/text.hpp"

namespace myclean::space {

namespace {

constexpr std::size_t hash_columns = 9;
constexpr std::size_t hash_rows = 8;

// The part of source pixel `index` (covering [index, index + 1)) inside [from, to).
double overlap(std::size_t index, double from, double to) {
  const double start = std::max(from, static_cast<double>(index));
  const double end = std::min(to, static_cast<double>(index) + 1.0);
  return std::max(0.0, end - start);
}

// The mean of the pixels of `image` the cell (`column`, `row`) of a hash_columns x hash_rows grid
// covers, each weighted by how much of it the cell covers.
double cell_mean(const GrayImage& image, std::size_t column, std::size_t row) {
  const double width = image.width;
  const double height = image.height;
  const double x0 = static_cast<double>(column) * width / hash_columns;
  const double x1 = static_cast<double>(column + 1) * width / hash_columns;
  const double y0 = static_cast<double>(row) * height / hash_rows;
  const double y1 = static_cast<double>(row + 1) * height / hash_rows;
  const auto first_x = static_cast<std::size_t>(std::floor(x0));
  const auto last_x = std::min<std::size_t>(image.width, static_cast<std::size_t>(std::ceil(x1)));
  const auto first_y = static_cast<std::size_t>(std::floor(y0));
  const auto last_y = std::min<std::size_t>(image.height, static_cast<std::size_t>(std::ceil(y1)));
  double sum = 0.0;
  double weight = 0.0;
  for (std::size_t y = first_y; y < last_y; ++y) {
    const double row_weight = overlap(y, y0, y1);
    for (std::size_t x = first_x; x < last_x; ++x) {
      const double w = row_weight * overlap(x, x0, x1);
      sum += w * image.pixels[(y * image.width) + x];
      weight += w;
    }
  }
  return weight > 0.0 ? sum / weight : 0.0;
}

// The root of `index` in the union-find forest `parent`, compressing the path.
std::size_t root_of(std::vector<std::size_t>& parent, std::size_t index) {
  while (parent[index] != index) {
    parent[index] = parent[parent[index]];
    index = parent[index];
  }
  return index;
}

bool is_candidate(const FileItem& file, const PhotoOptions& options) {
  if (file.link || file.placeholder || file.extra_link || file.size.count() == 0 ||
      file.size < options.min_size) {
    return false;
  }
  return std::ranges::find(options.extensions, lower_extension(file.name)) !=
         options.extensions.end();
}

std::uint64_t pixels_of(const Photo& photo) {
  return std::uint64_t{photo.width} * photo.height;
}

PhotoGroup make_group(std::vector<Photo> photos) {
  std::ranges::sort(photos, {}, &Photo::path);
  const auto keep = std::ranges::max_element(photos, [](const Photo& left, const Photo& right) {
    if (pixels_of(left) != pixels_of(right)) {
      return pixels_of(left) < pixels_of(right);
    }
    if (left.size_on_disk != right.size_on_disk) {
      return left.size_on_disk < right.size_on_disk;
    }
    return left.path > right.path;  // the first by path wins a tie
  });
  std::rotate(photos.begin(), keep, keep + 1);
  int distance = 0;
  std::uint64_t total = 0;
  for (std::size_t i = 0; i < photos.size(); ++i) {
    total += photos[i].size_on_disk.count();
    for (std::size_t j = i + 1; j < photos.size(); ++j) {
      distance = std::max(distance, hash_distance(photos[i].hash, photos[j].hash));
    }
  }
  const std::uint64_t kept = photos.front().size_on_disk.count();
  return PhotoGroup{
      .photos = std::move(photos), .distance = distance, .reclaimable = Bytes{total - kept}};
}

}  // namespace

PerceptualHash dhash(const GrayImage& image) {
  if (image.width == 0 || image.height == 0 ||
      image.pixels.size() < std::size_t{image.width} * image.height) {
    return 0;
  }
  std::array<double, hash_columns * hash_rows> cells{};
  for (std::size_t row = 0; row < hash_rows; ++row) {
    for (std::size_t column = 0; column < hash_columns; ++column) {
      cells.at((row * hash_columns) + column) = cell_mean(image, column, row);
    }
  }
  PerceptualHash hash = 0;
  for (std::size_t row = 0; row < hash_rows; ++row) {
    for (std::size_t column = 0; column + 1 < hash_columns; ++column) {
      if (cells.at((row * hash_columns) + column) > cells.at((row * hash_columns) + column + 1)) {
        hash |= PerceptualHash{1} << ((row * (hash_columns - 1)) + column);
      }
    }
  }
  return hash;
}

int hash_distance(PerceptualHash left, PerceptualHash right) {
  return std::popcount(left ^ right);
}

std::vector<std::vector<std::size_t>> group_hashes(std::span<const PerceptualHash> hashes,
                                                   int max_distance) {
  std::vector<std::size_t> parent(hashes.size());
  std::iota(parent.begin(), parent.end(), std::size_t{0});
  for (std::size_t i = 0; i < hashes.size(); ++i) {
    for (std::size_t j = i + 1; j < hashes.size(); ++j) {
      if (hash_distance(hashes[i], hashes[j]) <= max_distance) {
        const std::size_t a = root_of(parent, i);
        const std::size_t b = root_of(parent, j);
        if (a != b) {
          parent[std::max(a, b)] = std::min(a, b);
        }
      }
    }
  }
  // Members by root, in ascending order; a root is the smallest index of its group.
  std::vector<std::vector<std::size_t>> by_root(hashes.size());
  for (std::size_t i = 0; i < hashes.size(); ++i) {
    by_root[root_of(parent, i)].push_back(i);
  }
  std::vector<std::vector<std::size_t>> groups;
  for (std::vector<std::size_t>& members : by_root) {
    if (members.size() > 1) {
      groups.push_back(std::move(members));
    }
  }
  return groups;
}

PhotoReport find_similar_photos(std::span<const ScanResult> scans, const IImageDecoder& decoder,
                                const PhotoOptions& options, const std::stop_token& stop) {
  PhotoReport report;
  IdentitySet seen;
  std::vector<Photo> photos;
  for (const ScanResult& scan : scans) {
    for (const Folder& folder : scan.folders) {
      for (const FileItem& file : folder.files) {
        if (!is_candidate(file, options) || !seen.insert(file.identity)) {
          continue;
        }
        if (stop.stop_requested()) {
          report.cancelled = true;
          break;
        }
        const std::filesystem::path path = path_of(folder, file);
        const Result<DecodedImage> decoded = decoder.decode(path);
        if (!decoded) {
          report.not_analysed.push_back(ItemError{.path = path, .error = decoded.error()});
          continue;
        }
        photos.push_back(Photo{.path = path,
                               .size_on_disk = file.size_on_disk,
                               .modified = file.modified,
                               .identity = file.identity,
                               .width = decoded->width,
                               .height = decoded->height,
                               .hash = dhash(decoded->thumbnail)});
      }
    }
  }
  report.analysed = photos.size();
  std::vector<PerceptualHash> hashes;
  hashes.reserve(photos.size());
  for (const Photo& photo : photos) {
    hashes.push_back(photo.hash);
  }
  for (const std::vector<std::size_t>& members : group_hashes(hashes, options.max_distance)) {
    std::vector<Photo> group;
    group.reserve(members.size());
    for (const std::size_t index : members) {
      group.push_back(photos[index]);
    }
    report.groups.push_back(make_group(std::move(group)));
  }
  std::ranges::sort(report.groups, [](const PhotoGroup& left, const PhotoGroup& right) {
    if (left.reclaimable != right.reclaimable) {
      return left.reclaimable > right.reclaimable;
    }
    return left.photos.front().path < right.photos.front().path;
  });
  return report;
}

std::string to_hex(PerceptualHash hash) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string text(16, '0');
  for (std::size_t i = 0; i < text.size(); ++i) {
    text[text.size() - 1 - i] = digits[(hash >> (4 * i)) & 0x0FU];
  }
  return text;
}

}  // namespace myclean::space
