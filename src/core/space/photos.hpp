#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "core/model/file_identity.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"

// Near-duplicate photos (ntfs-index spec, "Photo Near-Duplicates"): each image is decoded, reduced
// to a 64-bit difference hash (dHash) and grouped with the images whose hashes differ in at most
// near_duplicate_bits bits, so a resized or re-encoded copy joins its original. Read-only: images
// are only decoded, and the groups are a report.
namespace myclean::space {

// A grayscale image: `pixels` holds width * height bytes, row by row, 0 black to 255 white.
struct GrayImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> pixels;
};

// What a decoder returns: a small grayscale copy of the image (any size; the hash scales it) and
// the size of the image itself.
struct DecodedImage {
  GrayImage thumbnail;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

// Decodes image files. Implemented by src/platform/image (Windows Imaging Component).
class IImageDecoder {
 public:
  virtual ~IImageDecoder() = default;
  IImageDecoder(const IImageDecoder&) = delete;
  IImageDecoder& operator=(const IImageDecoder&) = delete;
  IImageDecoder(IImageDecoder&&) = delete;
  IImageDecoder& operator=(IImageDecoder&&) = delete;

  // The first frame of `file`. Fails with refused for a link or a cloud placeholder, whose
  // content is never read, and with invalid_data for content no installed codec decodes.
  [[nodiscard]] virtual Result<DecodedImage> decode(const std::filesystem::path& file) const = 0;

 protected:
  IImageDecoder() = default;
};

using PerceptualHash = std::uint64_t;

// The largest number of differing hash bits of two images in one group.
inline constexpr int near_duplicate_bits = 6;

// The dHash of `image`: the image averaged down to 9 columns by 8 rows (each cell the mean of
// the pixels it covers), then bit row * 8 + column set when a cell is brighter than its right
// neighbour. An empty image hashes to 0.
[[nodiscard]] PerceptualHash dhash(const GrayImage& image);

// How many bits of `left` and `right` differ.
[[nodiscard]] int hash_distance(PerceptualHash left, PerceptualHash right);

// The groups of `hashes` (indices, each group in ascending order, groups by their first index):
// two hashes at most `max_distance` bits apart are in one group, and so is everything linked to
// either of them that way. Hashes linked to no other are in no group.
[[nodiscard]] std::vector<std::vector<std::size_t>> group_hashes(
    std::span<const PerceptualHash> hashes, int max_distance = near_duplicate_bits);

struct PhotoOptions {
  Bytes min_size{std::uint64_t{16} * 1024};  // smaller files (icons, thumbnails) are left out
  int max_distance = near_duplicate_bits;
  // Lower case with the dot: the formats Windows decodes, some only with a codec installed.
  std::vector<std::string> extensions{".jpg",  ".jpeg", ".jpe", ".jfif", ".png",
                                      ".bmp",  ".gif",  ".tif", ".tiff", ".heic",
                                      ".heif", ".webp", ".jxr", ".wdp",  ".dng"};
};

struct Photo {
  std::filesystem::path path;
  Bytes size_on_disk;
  WallTime modified;
  FileIdentity identity;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  PerceptualHash hash = 0;
};

struct PhotoGroup {
  // Two or more; the first is the copy to keep (the most pixels, then the largest file, then the
  // first by path), the others by path.
  std::vector<Photo> photos;
  int distance = 0;   // the most bits two photos of the group differ in
  Bytes reclaimable;  // freed by keeping only the first
};

struct PhotoReport {
  std::vector<PhotoGroup> groups;       // most reclaimable first
  std::vector<ItemError> not_analysed;  // files that could not be decoded, and why
  std::size_t analysed = 0;             // images decoded and hashed
  bool cancelled = false;               // stopped early: the groups of the images decoded so far
};

// The near-duplicate photos among the files of `scans` (one per folder or drive; overlapping
// scans are fine) whose extension is one of options.extensions and whose size is at least
// options.min_size. Links, cloud placeholders and further hard links of a file are skipped.
[[nodiscard]] PhotoReport find_similar_photos(std::span<const ScanResult> scans,
                                              const IImageDecoder& decoder,
                                              const PhotoOptions& options = {},
                                              const std::stop_token& stop = {});

// Sixteen lower-case hexadecimal digits of `hash`.
[[nodiscard]] std::string to_hex(PerceptualHash hash);

}  // namespace myclean::space
