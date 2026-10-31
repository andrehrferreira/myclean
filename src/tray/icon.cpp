#include "tray/icon.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/tray_model/tray_state.hpp"

namespace myclean::tray {

Result<wil::unique_hicon> make_icon(std::span<const tray_model::Argb> pixels, int size) {
  if (size <= 0 ||
      pixels.size() != static_cast<std::size_t>(size) * static_cast<std::size_t>(size)) {
    return fail(Errc::invalid_argument, "icon pixels do not match the size");
  }
  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(header);
  header.bV5Width = size;
  header.bV5Height = -size;  // top-down rows, as icon_pixels lays them out
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;

  void* bits = nullptr;
  const wil::unique_hdc_window screen(::GetDC(nullptr));
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): a V5 header starts as BITMAPINFO
  const auto* info = reinterpret_cast<const BITMAPINFO*>(&header);
  const wil::unique_hbitmap color(
      ::CreateDIBSection(screen.get(), info, DIB_RGB_COLORS, &bits, nullptr, 0));
  if (!color || bits == nullptr) {
    return fail(Errc::io_error, "CreateDIBSection failed");
  }
  std::ranges::copy(pixels, static_cast<tray_model::Argb*>(bits));
  // The alpha channel decides transparency; the mask only has to exist.
  const std::vector<unsigned char> mask_bits(
      static_cast<std::size_t>(((size + 15) / 16) * 2 * size), 0);
  const wil::unique_hbitmap mask(::CreateBitmap(size, size, 1, 1, mask_bits.data()));
  if (!mask) {
    return fail(Errc::io_error, "CreateBitmap failed");
  }
  ICONINFO icon_info{};
  icon_info.fIcon = TRUE;
  icon_info.hbmMask = mask.get();
  icon_info.hbmColor = color.get();
  wil::unique_hicon icon(::CreateIconIndirect(&icon_info));
  if (!icon) {
    return fail(Errc::io_error, "CreateIconIndirect failed");
  }
  return icon;
}

}  // namespace myclean::tray
