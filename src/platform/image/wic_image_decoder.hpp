#pragma once

#include <cstdint>
#include <filesystem>

#include "core/model/result.hpp"
#include "core/space/photos.hpp"

namespace myclean::platform {

// Decodes the first frame of an image with the Windows Imaging Component: every format a codec
// is installed for (JPEG, PNG, BMP, GIF, TIFF, JPEG XR, ICO built in; HEIF, WebP and camera raw
// with their Store codecs). The file is checked and opened as the content hasher opens it
// (platform::open_content: no link followed, no cloud file downloaded) and decoded from that
// handle, read-only. The thumbnail is the image scaled to thumbnail_width x thumbnail_height
// with Fant interpolation (an area average) in 8-bit gray. Initializes COM (multithreaded) on
// the calling thread for the call when it is not initialized yet.
class WicImageDecoder final : public space::IImageDecoder {
 public:
  static constexpr std::uint32_t thumbnail_width = 72;
  static constexpr std::uint32_t thumbnail_height = 64;

  WicImageDecoder() = default;

  [[nodiscard]] Result<space::DecodedImage> decode(
      const std::filesystem::path& file) const override;
};

}  // namespace myclean::platform
