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

Result<std::vector<tray_model::Argb>> app_icon_pixels(HINSTANCE module, int size) {
  if (size <= 0) {
    return fail(Errc::invalid_argument, "icon size must be positive");
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr): a resource id
  const wil::unique_hicon icon(static_cast<HICON>(
      ::LoadImageW(module, MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_DEFAULTCOLOR)));
  if (!icon) {
    return fail(Errc::not_found, "the module has no application icon");
  }
  ICONINFO info{};
  if (::GetIconInfo(icon.get(), &info) == FALSE) {
    return fail(Errc::io_error, "GetIconInfo failed");
  }
  const wil::unique_hbitmap color(info.hbmColor);
  const wil::unique_hbitmap mask(info.hbmMask);
  BITMAPINFO bitmap{};
  bitmap.bmiHeader.biSize = sizeof(bitmap.bmiHeader);
  bitmap.bmiHeader.biWidth = size;
  bitmap.bmiHeader.biHeight = -size;  // top-down rows
  bitmap.bmiHeader.biPlanes = 1;
  bitmap.bmiHeader.biBitCount = 32;
  bitmap.bmiHeader.biCompression = BI_RGB;
  std::vector<tray_model::Argb> pixels(static_cast<std::size_t>(size) *
                                       static_cast<std::size_t>(size));
  const wil::unique_hdc_window screen(::GetDC(nullptr));
  if (!color || ::GetDIBits(screen.get(), color.get(), 0, static_cast<UINT>(size), pixels.data(),
                            &bitmap, DIB_RGB_COLORS) != size) {
    return fail(Errc::io_error, "GetDIBits failed");
  }
  if (std::ranges::none_of(pixels, [](tray_model::Argb pixel) { return (pixel >> 24U) != 0; })) {
    return fail(Errc::invalid_argument, "the application icon has no alpha channel");
  }
  return pixels;
}

}  // namespace myclean::tray
