#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/rules/data_files.hpp"

namespace myclean::health {

// The rated limits of one drive model, from data/drive-specs.json. A limit without a source is
// left out of the file, so an absent value means "no rated limit known".
struct DriveSpec {
  std::string model;                // exactly as Windows reports it ("Samsung SSD 980 PRO 2TB")
  std::optional<double> rated_tbw;  // terabytes written (10^12 bytes)
  std::optional<std::int64_t> max_temperature_c;  // highest rated operating temperature
  std::string source;                             // where the values come from
};

class DriveSpecs {
 public:
  // The file name inside MyClean's data folder.
  static constexpr std::string_view file_name = "drive-specs.json";

  DriveSpecs() = default;

  // Parses the file's text. Fails with invalid_data, naming `file`, the entry and the field, on
  // invalid JSON, an unknown field, a missing model or source, a duplicate model or a limit
  // that is not a positive number.
  [[nodiscard]] static Result<DriveSpecs> parse(std::string_view text,
                                                const std::filesystem::path& file);
  // Reads and parses <data_dir>/drive-specs.json.
  [[nodiscard]] static Result<DriveSpecs> load(const rules::IDataFiles& files,
                                               const std::filesystem::path& data_dir);

  // The entry for `model`; null when the model is not listed.
  [[nodiscard]] const DriveSpec* find(std::string_view model) const;
  [[nodiscard]] const std::vector<DriveSpec>& entries() const { return entries_; }

 private:
  std::vector<DriveSpec> entries_;
};

}  // namespace myclean::health
