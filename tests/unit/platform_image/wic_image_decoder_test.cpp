#include "platform/image/wic_image_decoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <windows.h>
// Included after windows.h, which they need.
#include <objbase.h>
#include <wincodec.h>

#include "core/model/result.hpp"
#include "core/space/photos.hpp"

namespace myclean::platform {
namespace {

namespace fs = std::filesystem;

// A fresh folder %TEMP%\myclean-it\<unique>, removed with everything in it afterwards.
class Sandbox {
 public:
  Sandbox()
      : root_(fs::temp_directory_path() / "myclean-it" /
              ("image-" + std::to_string(::GetCurrentProcessId()) + "-" +
               std::to_string(::GetTickCount64()))) {
    fs::create_directories(root_);
  }
  ~Sandbox() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
  Sandbox(const Sandbox&) = delete;
  Sandbox& operator=(const Sandbox&) = delete;
  Sandbox(Sandbox&&) = delete;
  Sandbox& operator=(Sandbox&&) = delete;

  [[nodiscard]] const fs::path& root() const { return root_; }

 private:
  fs::path root_;
};

// The brightness of a smooth synthetic picture at (u, v) in [0, 1).
double brightness(double u, double v, double phase, double frequency) {
  return 128.0 + (70.0 * std::sin((frequency * 6.3 * u) + (3.1 * v) + phase)) +
         (40.0 * std::cos((frequency * 4.7 * v) - (2.3 * u) + (2.0 * phase)));
}

// Encodes the scene at width x height as 24-bit BGR into `file` with the WIC encoder of
// `container` (JPEG at `quality`, or PNG).
void encode(const fs::path& file, const GUID& container, std::uint32_t width, std::uint32_t height,
            double phase, double frequency, float quality = 0.9F) {
  const auto com = wil::CoInitializeEx(COINIT_MULTITHREADED);
  const auto factory = wil::CoCreateInstance<IWICImagingFactory>(CLSID_WICImagingFactory);
  wil::com_ptr<IWICStream> stream;
  THROW_IF_FAILED(factory->CreateStream(stream.put()));
  THROW_IF_FAILED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE));
  wil::com_ptr<IWICBitmapEncoder> encoder;
  THROW_IF_FAILED(factory->CreateEncoder(container, nullptr, encoder.put()));
  THROW_IF_FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
  wil::com_ptr<IWICBitmapFrameEncode> frame;
  wil::com_ptr<IPropertyBag2> options;
  THROW_IF_FAILED(encoder->CreateNewFrame(frame.put(), options.put()));
  if (container == GUID_ContainerFormatJpeg) {
    std::wstring name = L"ImageQuality";
    PROPBAG2 option{};
    option.pstrName = name.data();
    VARIANT value{};
    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access): VARIANT is a tagged union
    value.vt = VT_R4;
    value.fltVal = quality;
    // NOLINTEND(cppcoreguidelines-pro-type-union-access)
    THROW_IF_FAILED(options->Write(1, &option, &value));
  }
  THROW_IF_FAILED(frame->Initialize(options.get()));
  THROW_IF_FAILED(frame->SetSize(width, height));
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  THROW_IF_FAILED(frame->SetPixelFormat(&format));
  std::vector<BYTE> pixels(std::size_t{width} * height * 3);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const double value = brightness(static_cast<double>(x) / width,
                                      static_cast<double>(y) / height, phase, frequency);
      const auto level = static_cast<BYTE>(std::lround(std::clamp(value, 0.0, 255.0)));
      const std::size_t at = ((std::size_t{y} * width) + x) * 3;
      pixels[at] = level;
      pixels[at + 1] = level;
      pixels[at + 2] = static_cast<BYTE>(255 - level);
    }
  }
  THROW_IF_FAILED(
      frame->WritePixels(height, width * 3, static_cast<UINT>(pixels.size()), pixels.data()));
  THROW_IF_FAILED(frame->Commit());
  THROW_IF_FAILED(encoder->Commit());
}

space::PerceptualHash hash_of(const WicImageDecoder& decoder, const fs::path& file) {
  const Result<space::DecodedImage> image = decoder.decode(file);
  EXPECT_TRUE(image.has_value()) << (image ? "" : image.error().message);
  return image ? space::dhash(image->thumbnail) : 0;
}

TEST(WicImageDecoder, DecodesSizeAndGrayThumbnail) {
  const Sandbox sandbox;
  const fs::path photo = sandbox.root() / "photo.jpg";
  encode(photo, GUID_ContainerFormatJpeg, 640, 480, 0.4, 1.0);

  const WicImageDecoder decoder;
  const Result<space::DecodedImage> image = decoder.decode(photo);

  ASSERT_TRUE(image.has_value()) << image.error().message;
  EXPECT_EQ(image->width, 640U);
  EXPECT_EQ(image->height, 480U);
  EXPECT_EQ(image->thumbnail.width, WicImageDecoder::thumbnail_width);
  EXPECT_EQ(image->thumbnail.height, WicImageDecoder::thumbnail_height);
  EXPECT_EQ(image->thumbnail.pixels.size(),
            std::size_t{WicImageDecoder::thumbnail_width} * WicImageDecoder::thumbnail_height);
}

TEST(WicImageDecoder, AResizedReencodedCopyHashesWithinSixBits) {
  const Sandbox sandbox;
  const fs::path original = sandbox.root() / "img.jpg";
  const fs::path small_jpeg = sandbox.root() / "img_small.jpg";
  const fs::path small_png = sandbox.root() / "img_small.png";
  encode(original, GUID_ContainerFormatJpeg, 1600, 1200, 0.4, 1.0, 0.95F);
  encode(small_jpeg, GUID_ContainerFormatJpeg, 800, 600, 0.4, 1.0, 0.5F);
  encode(small_png, GUID_ContainerFormatPng, 400, 300, 0.4, 1.0);

  const WicImageDecoder decoder;
  const space::PerceptualHash hash = hash_of(decoder, original);
  EXPECT_LE(space::hash_distance(hash, hash_of(decoder, small_jpeg)), space::near_duplicate_bits);
  EXPECT_LE(space::hash_distance(hash, hash_of(decoder, small_png)), space::near_duplicate_bits);
}

TEST(WicImageDecoder, DifferentPicturesHashFarApart) {
  const Sandbox sandbox;
  const fs::path first = sandbox.root() / "first.jpg";
  const fs::path other = sandbox.root() / "other.jpg";
  encode(first, GUID_ContainerFormatJpeg, 800, 600, 0.4, 1.0);
  encode(other, GUID_ContainerFormatJpeg, 800, 600, 3.0, 1.7);

  const WicImageDecoder decoder;
  EXPECT_GT(space::hash_distance(hash_of(decoder, first), hash_of(decoder, other)),
            space::near_duplicate_bits);
}

TEST(WicImageDecoder, RefusesWhatNoCodecDecodesAndMissingFiles) {
  const Sandbox sandbox;
  const fs::path fake = sandbox.root() / "not-a-photo.jpg";
  std::ofstream(fake) << "this is text with a .jpg name, not an image";

  const WicImageDecoder decoder;
  const Result<space::DecodedImage> undecodable = decoder.decode(fake);
  ASSERT_FALSE(undecodable.has_value());
  EXPECT_EQ(undecodable.error().code, Errc::invalid_data) << undecodable.error().message;

  const Result<space::DecodedImage> missing = decoder.decode(sandbox.root() / "gone.jpg");
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, Errc::not_found);
}

}  // namespace
}  // namespace myclean::platform
