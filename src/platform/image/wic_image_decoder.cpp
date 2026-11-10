#include "platform/image/wic_image_decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <wil/com.h>
#include <wil/resource.h>
#include <windows.h>
// Included after windows.h, which it needs.
#include <wincodec.h>

#include "core/model/result.hpp"
#include "core/space/photos.hpp"
#include "platform/scan/file_content.hpp"
#include "platform/scan/scan_support.hpp"

namespace myclean::platform {

namespace {

// "<action> <file>: HRESULT 0x...", as invalid_data for what no codec decodes and io_error else.
Error wic_error(HRESULT result, std::string_view action, const std::filesystem::path& file) {
  const bool undecodable = result == WINCODEC_ERR_COMPONENTNOTFOUND ||
                           result == WINCODEC_ERR_UNKNOWNIMAGEFORMAT ||
                           result == WINCODEC_ERR_BADHEADER || result == WINCODEC_ERR_BADIMAGE ||
                           result == WINCODEC_ERR_BADSTREAMDATA ||
                           result == WINCODEC_ERR_STREAMREAD || result == WINCODEC_ERR_FRAMEMISSING;
  return Error{.code = undecodable ? Errc::invalid_data : Errc::io_error,
               .message = std::format("{} {}: HRESULT 0x{:08X}", action, scan_support::utf8(file),
                                      static_cast<std::uint32_t>(result))};
}

}  // namespace

Result<space::DecodedImage> WicImageDecoder::decode(const std::filesystem::path& file) const {
  Result<wil::unique_hfile> handle = open_content(file);
  if (!handle) {
    return tl::unexpected(handle.error());
  }
  const HRESULT initialized = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
    return tl::unexpected(wic_error(initialized, "initializing COM for", file));
  }
  const auto uninitialize = wil::scope_exit([&] {
    if (SUCCEEDED(initialized)) {
      ::CoUninitialize();
    }
  });

  wil::com_ptr_nothrow<IWICImagingFactory> factory;
  if (const HRESULT result = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()));
      FAILED(result)) {
    return tl::unexpected(wic_error(result, "creating the imaging factory for", file));
  }
  wil::com_ptr_nothrow<IWICBitmapDecoder> decoder;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the API takes it as ULONG_PTR
  const auto handle_value = reinterpret_cast<ULONG_PTR>(handle->get());
  if (const HRESULT result = factory->CreateDecoderFromFileHandle(
          handle_value, nullptr, WICDecodeMetadataCacheOnDemand, decoder.put());
      FAILED(result)) {
    return tl::unexpected(wic_error(result, "no codec decodes", file));
  }
  wil::com_ptr_nothrow<IWICBitmapFrameDecode> frame;
  if (const HRESULT result = decoder->GetFrame(0, frame.put()); FAILED(result)) {
    return tl::unexpected(wic_error(result, "cannot decode the first frame of", file));
  }
  UINT width = 0;
  UINT height = 0;
  if (const HRESULT result = frame->GetSize(&width, &height); FAILED(result)) {
    return tl::unexpected(wic_error(result, "cannot read the size of", file));
  }
  if (width == 0 || height == 0) {
    return fail(Errc::invalid_data, "empty image: " + scan_support::utf8(file));
  }
  wil::com_ptr_nothrow<IWICBitmapScaler> scaler;
  if (const HRESULT result = factory->CreateBitmapScaler(scaler.put()); FAILED(result)) {
    return tl::unexpected(wic_error(result, "creating a scaler for", file));
  }
  if (const HRESULT result = scaler->Initialize(frame.get(), thumbnail_width, thumbnail_height,
                                                WICBitmapInterpolationModeFant);
      FAILED(result)) {
    return tl::unexpected(wic_error(result, "cannot scale", file));
  }
  wil::com_ptr_nothrow<IWICFormatConverter> converter;
  if (const HRESULT result = factory->CreateFormatConverter(converter.put()); FAILED(result)) {
    return tl::unexpected(wic_error(result, "creating a format converter for", file));
  }
  if (const HRESULT result =
          converter->Initialize(scaler.get(), GUID_WICPixelFormat8bppGray, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeCustom);
      FAILED(result)) {
    return tl::unexpected(wic_error(result, "cannot convert to gray", file));
  }
  space::DecodedImage image{
      .thumbnail = {.width = thumbnail_width, .height = thumbnail_height, .pixels = {}},
      .width = width,
      .height = height};
  image.thumbnail.pixels.resize(std::size_t{thumbnail_width} * thumbnail_height);
  if (const HRESULT result = converter->CopyPixels(nullptr, thumbnail_width,
                                                   static_cast<UINT>(image.thumbnail.pixels.size()),
                                                   image.thumbnail.pixels.data());
      FAILED(result)) {
    return tl::unexpected(wic_error(result, "cannot decode", file));
  }
  return image;
}

}  // namespace myclean::platform
