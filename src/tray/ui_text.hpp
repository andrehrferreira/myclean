#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "core/model/units.hpp"
#include "core/tray_model/format.hpp"

// The dashboard's palette and text helpers, shared by its sections. Inside an ImGui frame only.
namespace myclean::tray::ui {

// The palette, as 0xRRGGBB.
namespace color {
inline constexpr unsigned background = 0x111418;
inline constexpr unsigned panel = 0x181C21;
inline constexpr unsigned raised = 0x20252B;
inline constexpr unsigned hover = 0x2A3038;
inline constexpr unsigned border = 0x2A2F36;
inline constexpr unsigned text = 0xE6E8EB;
inline constexpr unsigned muted = 0x8B939C;
inline constexpr unsigned accent = 0x4C9AFF;
inline constexpr unsigned good = 0x2EB872;
inline constexpr unsigned warning = 0xE8B21E;
inline constexpr unsigned bad = 0xE5484D;
}  // namespace color

inline ImVec4 rgb(unsigned value, float alpha = 1.0F) {
  return {static_cast<float>((value >> 16U) & 0xFFU) / 255.0F,
          static_cast<float>((value >> 8U) & 0xFFU) / 255.0F,
          static_cast<float>(value & 0xFFU) / 255.0F, alpha};
}

inline void text(std::string_view value) {
  ImGui::TextUnformatted(value.data(), value.data() + value.size());
}

inline void colored_text(unsigned value_color, std::string_view value) {
  ImGui::PushStyleColor(ImGuiCol_Text, rgb(value_color));
  text(value);
  ImGui::PopStyleColor();
}

inline void muted_text(std::string_view value) {
  colored_text(color::muted, value);
}

// Text that wraps at the right edge of the current column or window.
inline void wrapped_text(unsigned value_color, std::string_view value) {
  ImGui::PushTextWrapPos(0.0F);
  colored_text(value_color, value);
  ImGui::PopTextWrapPos();
}

inline std::string join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string joined;
  for (const std::string& part : parts) {
    joined += (joined.empty() ? "" : std::string(separator)) + part;
  }
  return joined;
}

inline std::string local_date_time(const std::chrono::time_zone& zone, WallTime time) {
  return tray_model::format_date_time(
      std::chrono::floor<std::chrono::seconds>(zone.to_local(time)));
}

}  // namespace myclean::tray::ui
