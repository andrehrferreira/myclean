#include "core/tray_model/format.hpp"

#include <chrono>
#include <limits>
#include <optional>

#include <gtest/gtest.h>

namespace myclean::tray_model {
namespace {

using namespace std::chrono_literals;

constexpr double kib = 1024.0;
constexpr double gib = kib * kib * kib;

TEST(FormatTest, BytesUseBinaryUnitsWithThreeSignificantDigits) {
  EXPECT_EQ(format_bytes(0.0), "0 B");
  EXPECT_EQ(format_bytes(512.0), "512 B");
  EXPECT_EQ(format_bytes(1536.0), "1.50 KB");
  EXPECT_EQ(format_bytes(84 * gib), "84.0 GB");
  EXPECT_EQ(format_bytes(952 * gib), "952 GB");
  EXPECT_EQ(format_bytes(1863 * gib), "1.82 TB");
}

TEST(FormatTest, BytesNeverPrintAThousandOfAUnit) {
  EXPECT_EQ(format_bytes(1023.9 * kib), "1.00 MB");
  EXPECT_EQ(format_bytes(999.4 * kib), "999 KB");
}

TEST(FormatTest, MissingOrInvalidValuesAreNotAvailable) {
  EXPECT_EQ(format_bytes(std::nullopt), "n/a");
  EXPECT_EQ(format_bytes(-1.0), "n/a");
  EXPECT_EQ(format_bytes(std::numeric_limits<double>::quiet_NaN()), "n/a");
  EXPECT_EQ(format_rate(std::nullopt), "n/a");
  EXPECT_EQ(format_percent(std::nullopt), "n/a");
  EXPECT_EQ(format_milliseconds(std::nullopt), "n/a");
  EXPECT_EQ(format_count(std::nullopt), "n/a");
}

TEST(FormatTest, RatesArePerSecond) {
  EXPECT_EQ(format_rate(12.5 * kib * kib), "12.5 MB/s");
  EXPECT_EQ(format_rate(0.0), "0 B/s");
}

TEST(FormatTest, PercentHasADecimalOnlyBelowOne) {
  EXPECT_EQ(format_percent(42.4), "42 %");
  EXPECT_EQ(format_percent(0.0), "0 %");
  EXPECT_EQ(format_percent(0.46), "0.5 %");
  EXPECT_EQ(format_percent(100.0), "100 %");
}

TEST(FormatTest, MillisecondsAndCounts) {
  EXPECT_EQ(format_milliseconds(0.123), "0.12 ms");
  EXPECT_EQ(format_milliseconds(4.5), "4.50 ms");
  EXPECT_EQ(format_milliseconds(120.0), "120 ms");
  EXPECT_EQ(format_count(3.0), "3");
  EXPECT_EQ(format_count(0.4), "0.4");
  EXPECT_EQ(format_count(1520.02), "1520");
}

TEST(FormatTest, DaysUntilFull) {
  EXPECT_EQ(format_days(40.5), "40 days");
  EXPECT_EQ(format_days(1.9), "1 day");
  EXPECT_EQ(format_days(0.3), "less than 1 day");
}

TEST(FormatTest, TimeOfDay) {
  const std::chrono::local_seconds time{std::chrono::local_days{} + 14h + 23min + 5s};
  EXPECT_EQ(format_time_of_day(time), "14:23:05");
}

}  // namespace
}  // namespace myclean::tray_model
