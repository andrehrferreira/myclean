#include "core/space/photos.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <stop_token>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/json.hpp"
#include "core/space/scanner.hpp"
#include "space_fakes.hpp"

namespace myclean::space {
namespace {

using testing::FakeDirectoryReader;
using testing::scan_ok;
namespace fs = std::filesystem;

// A smooth synthetic picture: brightness waves whose phase and frequency make different scenes.
GrayImage scene(std::uint32_t width, std::uint32_t height, double phase, double frequency) {
  GrayImage image{.width = width, .height = height, .pixels = {}};
  image.pixels.resize(std::size_t{width} * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const double u = static_cast<double>(x) / width;
      const double v = static_cast<double>(y) / height;
      const double value = 128.0 + (70.0 * std::sin((frequency * 6.3 * u) + (3.1 * v) + phase)) +
                           (40.0 * std::cos((frequency * 4.7 * v) - (2.3 * u) + (2.0 * phase)));
      image.pixels[(std::size_t{y} * width) + x] =
          static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 255.0)));
    }
  }
  return image;
}

// `image` at half its width and height, each pixel the mean of the four it replaces.
GrayImage half_size(const GrayImage& image) {
  GrayImage half{.width = image.width / 2, .height = image.height / 2, .pixels = {}};
  half.pixels.resize(std::size_t{half.width} * half.height);
  for (std::uint32_t y = 0; y < half.height; ++y) {
    for (std::uint32_t x = 0; x < half.width; ++x) {
      const auto at = [&](std::uint32_t xx, std::uint32_t yy) {
        return static_cast<int>(image.pixels[(std::size_t{yy} * image.width) + xx]);
      };
      const int sum = at(2 * x, 2 * y) + at((2 * x) + 1, 2 * y) + at(2 * x, (2 * y) + 1) +
                      at((2 * x) + 1, (2 * y) + 1);
      half.pixels[(std::size_t{y} * half.width) + x] = static_cast<std::uint8_t>(sum / 4);
    }
  }
  return half;
}

// `image` with a deterministic error of up to `amplitude` levels per pixel, as lossy re-encoding
// leaves.
GrayImage with_noise(GrayImage image, int amplitude) {
  std::uint32_t state = 12345;
  for (std::uint8_t& pixel : image.pixels) {
    state = (state * 1103515245U) + 12345U;
    const int error =
        static_cast<int>((state >> 16U) % static_cast<std::uint32_t>((2 * amplitude) + 1)) -
        amplitude;
    pixel = static_cast<std::uint8_t>(std::clamp(static_cast<int>(pixel) + error, 0, 255));
  }
  return image;
}

class FakeDecoder final : public IImageDecoder {
 public:
  std::map<fs::path, DecodedImage> images;
  mutable std::vector<fs::path> decoded;

  [[nodiscard]] Result<DecodedImage> decode(const fs::path& file) const override {
    decoded.push_back(file);
    const auto image = images.find(file);
    if (image == images.end()) {
      return fail(Errc::invalid_data, "no codec decodes this file");
    }
    return image->second;
  }
};

TEST(Photos, ResizedAndReencodedCopyHashesWithinSixBits) {
  const GrayImage original = scene(640, 480, 0.4, 1.0);
  const GrayImage copy = with_noise(half_size(original), 6);
  EXPECT_LE(hash_distance(dhash(original), dhash(copy)), near_duplicate_bits);
}

TEST(Photos, DifferentScenesHashFarApart) {
  const PerceptualHash first = dhash(scene(640, 480, 0.4, 1.0));
  for (const double phase : {1.9, 3.0, 4.4}) {
    EXPECT_GT(hash_distance(first, dhash(scene(640, 480, phase, 1.7))), near_duplicate_bits)
        << "phase " << phase;
  }
}

TEST(Photos, HashComparesNeighbouringCellsLeftToRight) {
  // Brightness falling from left to right: every cell is brighter than its right neighbour.
  GrayImage falling{.width = 9, .height = 8, .pixels = {}};
  for (std::uint32_t y = 0; y < 8; ++y) {
    for (std::uint32_t x = 0; x < 9; ++x) {
      falling.pixels.push_back(static_cast<std::uint8_t>(250 - (x * 25)));
    }
  }
  EXPECT_EQ(dhash(falling), ~PerceptualHash{0});
  const GrayImage flat{.width = 90, .height = 80, .pixels = std::vector<std::uint8_t>(7200, 100)};
  EXPECT_EQ(dhash(flat), PerceptualHash{0});
  EXPECT_EQ(dhash(GrayImage{}), PerceptualHash{0});
}

TEST(Photos, GroupsHashesAtMostSixBitsApartOnly) {
  const PerceptualHash base = 0x0123456789ABCDEFULL;
  const std::vector<PerceptualHash> hashes = {
      base,
      base ^ 0x3FULL,               // 6 bits from base: grouped
      base ^ 0xFFF000000000ULL,     // 12 bits from base: not grouped
      base ^ 0xFFF000000000ULL ^ 1  // 1 bit from the previous one: grouped with it
  };
  const std::vector<std::vector<std::size_t>> groups = group_hashes(hashes);
  ASSERT_EQ(groups.size(), 2U);
  EXPECT_EQ(groups[0], (std::vector<std::size_t>{0, 1}));
  EXPECT_EQ(groups[1], (std::vector<std::size_t>{2, 3}));
  EXPECT_EQ(hash_distance(base, base ^ 0xFFF000000000ULL), 12);
}

TEST(Photos, GroupsAResizedCopyOnAnotherDriveAndKeepsTheLargerImage) {
  FakeDirectoryReader d_reader;
  FakeDirectoryReader k_reader;
  const fs::path d = L"D:\\photos";
  const fs::path k = L"K:\\old";
  d_reader.listings[d] = {testing::file("img.jpg", 900'000, 1),
                          testing::file("beach.jpg", 800'000, 2)};
  ListedEntry small = testing::file("img_small.jpg", 200'000, 3);
  small.identity.volume_serial = 11;
  k_reader.listings[k] = {small};

  FakeDecoder decoder;
  const GrayImage original = scene(640, 480, 0.4, 1.0);
  decoder.images[d / "img.jpg"] =
      DecodedImage{.thumbnail = original, .width = 4000, .height = 3000};
  decoder.images[k / "img_small.jpg"] =
      DecodedImage{.thumbnail = with_noise(half_size(original), 6), .width = 2000, .height = 1500};
  decoder.images[d / "beach.jpg"] =
      DecodedImage{.thumbnail = scene(640, 480, 3.0, 1.7), .width = 4000, .height = 3000};

  const std::vector<ScanResult> scans = {scan_ok(d_reader, d), scan_ok(k_reader, k)};
  const PhotoReport report = find_similar_photos(scans, decoder);

  EXPECT_EQ(report.analysed, 3U);
  ASSERT_EQ(report.groups.size(), 1U);
  const PhotoGroup& group = report.groups[0];
  ASSERT_EQ(group.photos.size(), 2U);
  EXPECT_EQ(group.photos[0].path, d / "img.jpg");  // the most pixels: kept
  EXPECT_EQ(group.photos[1].path, k / "img_small.jpg");
  EXPECT_EQ(group.reclaimable, Bytes{200'000});
  EXPECT_LE(group.distance, near_duplicate_bits);
  EXPECT_TRUE(report.not_analysed.empty());

  const nlohmann::json json = report;
  EXPECT_EQ(json.at("groups").at(0).at("files").at(0).at("keep"), true);
  EXPECT_EQ(json.at("groups").at(0).at("files").at(1).at("keep"), false);
  EXPECT_EQ(json.at("groups").at(0).at("files").at(0).at("width"), 4000);
  EXPECT_EQ(json.at("reclaimable"), 200'000);
  EXPECT_EQ(json.at("analysed"), 3);
}

TEST(Photos, ListsAnUndecodableFileAsNotAnalysed) {
  FakeDirectoryReader reader;
  const fs::path root = L"D:\\photos";
  reader.listings[root] = {testing::file("broken.jpg", 50'000, 1),
                           testing::file("good.jpg", 50'000, 2)};
  FakeDecoder decoder;
  decoder.images[root / "good.jpg"] =
      DecodedImage{.thumbnail = scene(64, 48, 0.4, 1.0), .width = 64, .height = 48};

  const std::vector<ScanResult> scans = {scan_ok(reader, root)};
  const PhotoReport report = find_similar_photos(scans, decoder);

  EXPECT_TRUE(report.groups.empty());
  EXPECT_EQ(report.analysed, 1U);
  ASSERT_EQ(report.not_analysed.size(), 1U);
  EXPECT_EQ(report.not_analysed[0].path, root / "broken.jpg");
  EXPECT_EQ(report.not_analysed[0].error.code, Errc::invalid_data);
  const nlohmann::json json = report;
  EXPECT_EQ(json.at("not_analysed").at(0).at("error").at("code"), "invalid_data");
}

TEST(Photos, DecodesOnlyPhotosOfTheMinimumSizeOncePerFile) {
  FakeDirectoryReader reader;
  const fs::path root = L"D:\\pictures";
  const ListedEntry link = testing::junction("link.jpg");
  ListedEntry cloud = testing::file("cloud.jpg", 90'000, 4);
  cloud.attributes.recall_on_data_access = true;
  reader.listings[root] = {testing::file("photo.JPG", 90'000, 1),
                           testing::file("hardlink.jpg", 90'000, 1),
                           testing::file("icon.png", 2'000, 2),
                           testing::file("notes.txt", 90'000, 3),
                           cloud,
                           link};
  const FakeDecoder decoder;

  const std::vector<ScanResult> scans = {scan_ok(reader, root)};
  static_cast<void>(find_similar_photos(scans, decoder));

  EXPECT_EQ(decoder.decoded, (std::vector<fs::path>{root / "hardlink.jpg"}))
      << "photo.JPG and hardlink.jpg are one file; the first listed by name is decoded";
}

TEST(Photos, StopsWhenAskedAndSaysSo) {
  FakeDirectoryReader reader;
  const fs::path root = L"D:\\photos";
  reader.listings[root] = {testing::file("a.jpg", 50'000, 1)};
  const FakeDecoder decoder;
  std::stop_source stop;
  stop.request_stop();

  const std::vector<ScanResult> scans = {scan_ok(reader, root)};
  const PhotoReport report = find_similar_photos(scans, decoder, {}, stop.get_token());

  EXPECT_TRUE(report.cancelled);
  EXPECT_TRUE(decoder.decoded.empty());
}

TEST(Photos, HexIsSixteenDigits) {
  EXPECT_EQ(to_hex(PerceptualHash{0xABCULL}), "0000000000000abc");
}

}  // namespace
}  // namespace myclean::space
