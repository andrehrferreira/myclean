#include "tray/icon.hpp"

#include <vector>

#include <gtest/gtest.h>
#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/tray_model/tray_state.hpp"

namespace myclean::tray {
namespace {

// Creating an icon only allocates GDI objects; nothing is displayed.
TEST(IconTest, MakesA32BitIconOfTheRequestedSize) {
  for (const int size : {16, 20, 24, 32}) {
    const tray_model::TrayStatus status{.state = tray_model::TrayState::alert,
                                        .severity = tray_model::AlertSeverity::warning,
                                        .badge = true};
    const Result<wil::unique_hicon> icon = make_icon(tray_model::icon_pixels(status, size), size);
    ASSERT_TRUE(icon.has_value()) << icon.error().message;
    ICONINFO info{};
    ASSERT_NE(::GetIconInfo(icon->get(), &info), FALSE);
    const wil::unique_hbitmap color(info.hbmColor);
    const wil::unique_hbitmap mask(info.hbmMask);
    EXPECT_NE(info.fIcon, FALSE);
    BITMAP bitmap{};
    ASSERT_NE(::GetObjectW(color.get(), sizeof(bitmap), &bitmap), 0);
    EXPECT_EQ(bitmap.bmWidth, size);
    EXPECT_EQ(bitmap.bmHeight, size);
    EXPECT_EQ(bitmap.bmBitsPixel, 32);
  }
}

TEST(IconTest, RefusesPixelsThatDoNotMatchTheSize) {
  const std::vector<tray_model::Argb> pixels(10, 0);
  const Result<wil::unique_hicon> icon = make_icon(pixels, 16);
  ASSERT_FALSE(icon.has_value());
  EXPECT_EQ(icon.error().code, Errc::invalid_argument);
  EXPECT_FALSE(make_icon({}, 0).has_value());
}

}  // namespace
}  // namespace myclean::tray
