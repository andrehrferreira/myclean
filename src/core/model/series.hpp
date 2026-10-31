#pragma once

#include <compare>
#include <string>
#include <vector>

#include "core/model/units.hpp"

namespace myclean {

// One measurement of one metric for one entity.
struct Sample {
  WallTime time;
  double value = 0.0;
};

// What a time series measures: a metric ("cpu.core") for an entity ("3", a disk id, ...).
struct SeriesKey {
  std::string metric;
  std::string entity;

  friend auto operator<=>(const SeriesKey&, const SeriesKey&) = default;
};

// Samples of one key, oldest first.
struct Series {
  SeriesKey key;
  std::vector<Sample> samples;
};

}  // namespace myclean
