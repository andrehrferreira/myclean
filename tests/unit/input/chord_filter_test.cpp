#include "core/input/chord_filter.hpp"

#include <cstdint>
#include <optional>

#include <gtest/gtest.h>

#include "core/input/incident.hpp"

namespace myclean::input {
namespace {

constexpr std::uint16_t vk_control = 0x11;
constexpr std::uint16_t vk_menu = 0x12;
constexpr std::uint16_t vk_shift = 0x10;
constexpr std::uint16_t vk_lcontrol = 0xA2;
constexpr std::uint16_t vk_rcontrol = 0xA3;
constexpr std::uint16_t vk_rmenu = 0xA5;
constexpr std::uint16_t vk_a = 0x41;
constexpr std::uint16_t vk_c = 0x43;
constexpr std::uint16_t vk_v = 0x56;
constexpr std::uint16_t vk_x = 0x58;

std::optional<Chord> press(ChordFilter& filter, std::uint16_t key, bool extended = false) {
  return filter.on_key(key, extended, false);
}

std::optional<Chord> release(ChordFilter& filter, std::uint16_t key, bool extended = false) {
  return filter.on_key(key, extended, true);
}

TEST(ChordFilter, CtrlCIsCopyAndCtrlVIsPaste) {
  ChordFilter filter;
  EXPECT_EQ(press(filter, vk_control), std::nullopt);
  EXPECT_EQ(press(filter, vk_c), Chord::copy);
  EXPECT_EQ(release(filter, vk_c), std::nullopt);
  EXPECT_EQ(press(filter, vk_v), Chord::paste);
}

TEST(ChordFilter, LettersWithoutCtrlPassNothing) {
  ChordFilter filter;
  for (std::uint16_t key = 0x30; key <= 0x5A; ++key) {
    EXPECT_EQ(press(filter, key), std::nullopt) << key;
    EXPECT_EQ(release(filter, key), std::nullopt) << key;
  }
}

TEST(ChordFilter, OnlyCAndVWithCtrlPassAnything) {
  ChordFilter filter;
  press(filter, vk_control);
  int chords = 0;
  for (std::uint16_t key = 0x01; key < 0xFF; ++key) {
    if (key == vk_control || key == vk_lcontrol || key == vk_rcontrol || key == vk_menu ||
        key == 0xA4 || key == vk_rmenu) {
      continue;
    }
    if (press(filter, key)) {
      ++chords;
    }
    release(filter, key);
  }
  EXPECT_EQ(chords, 2);
}

TEST(ChordFilter, OtherLetterBetweenCtrlAndCDoesNotMatter) {
  ChordFilter filter;
  press(filter, vk_control);
  EXPECT_EQ(press(filter, vk_a), std::nullopt);
  EXPECT_EQ(release(filter, vk_a), std::nullopt);
  EXPECT_EQ(press(filter, vk_x), std::nullopt);
  EXPECT_EQ(press(filter, vk_c), Chord::copy);
}

TEST(ChordFilter, AutoRepeatOfHeldKeyIsOneChord) {
  ChordFilter filter;
  press(filter, vk_control);
  EXPECT_EQ(press(filter, vk_v), Chord::paste);
  EXPECT_EQ(press(filter, vk_v), std::nullopt);
  EXPECT_EQ(press(filter, vk_v), std::nullopt);
  release(filter, vk_v);
  EXPECT_EQ(press(filter, vk_v), Chord::paste);
}

TEST(ChordFilter, ReleasedCtrlEndsTheChord) {
  ChordFilter filter;
  press(filter, vk_control);
  release(filter, vk_control);
  EXPECT_EQ(press(filter, vk_c), std::nullopt);
}

TEST(ChordFilter, RightCtrlCountsAndBothSidesAreTracked) {
  ChordFilter filter;
  press(filter, vk_control);
  press(filter, vk_control, true);
  release(filter, vk_control);  // left released, right still held
  EXPECT_EQ(press(filter, vk_c), Chord::copy);
  release(filter, vk_c);
  release(filter, vk_control, true);
  EXPECT_EQ(press(filter, vk_c), std::nullopt);
}

TEST(ChordFilter, SidedControlCodesAreAccepted) {
  ChordFilter filter;
  press(filter, vk_rcontrol);
  EXPECT_EQ(press(filter, vk_v), Chord::paste);
  release(filter, vk_v);
  release(filter, vk_rcontrol);
  press(filter, vk_lcontrol);
  EXPECT_EQ(press(filter, vk_c), Chord::copy);
}

TEST(ChordFilter, AltGrIsNotCopy) {
  ChordFilter filter;
  press(filter, vk_control);
  press(filter, vk_menu, true);  // AltGr sends left Ctrl + right Alt
  EXPECT_EQ(press(filter, vk_c), std::nullopt);
  release(filter, vk_c);
  release(filter, vk_menu, true);
  EXPECT_EQ(press(filter, vk_c), Chord::copy);
}

TEST(ChordFilter, ShiftDoesNotBlockTheChord) {
  ChordFilter filter;
  press(filter, vk_control);
  press(filter, vk_shift);
  EXPECT_EQ(press(filter, vk_v), Chord::paste);
}

}  // namespace
}  // namespace myclean::input
