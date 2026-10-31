#include "core/health/drive_specs.hpp"

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/rules/data_files.hpp"

namespace myclean::health {
namespace {

Result<DriveSpecs> parse(const std::string& text) {
  return DriveSpecs::parse(text, "drive-specs.json");
}

TEST(DriveSpecs, ShippedFileCoversTheEightReferenceDrivesWithSources) {
  const rules::DiskDataFiles files;
  const Result<DriveSpecs> specs = DriveSpecs::load(files, MYCLEAN_HEALTH_DATA);
  ASSERT_TRUE(specs.has_value()) << specs.error().message;

  for (const char* model :
       {"Samsung SSD 970 PRO 1TB", "Samsung SSD 980 PRO 2TB", "WD_BLACK SN7100 4TB", "CT4000P3SSD8",
        "Sabrent Rocket 4.0 2TB", "ROG ESD-S1C", "WDC WD101EFBX-68B0AN0"}) {
    const DriveSpec* spec = specs->find(model);
    ASSERT_NE(spec, nullptr) << model;
    EXPECT_FALSE(spec->source.empty()) << model;
  }
  EXPECT_EQ(specs->find("Samsung SSD 980 PRO 2TB")->rated_tbw, 1200.0);
  EXPECT_EQ(specs->find("WDC WD101EFBX-68B0AN0")->max_temperature_c, 65);
  EXPECT_FALSE(specs->find("WDC WD101EFBX-68B0AN0")->rated_tbw.has_value());
  EXPECT_FALSE(specs->find("CT4000P3SSD8")->max_temperature_c.has_value());
  EXPECT_EQ(specs->find("Unknown Model"), nullptr);
}

TEST(DriveSpecs, InvalidFilesAreRejectedWithTheEntryAndField) {
  struct Case {
    const char* text;
    const char* message;
  };
  for (const Case& bad : {
           Case{.text = "[]",
                .message = "drive-specs.json: must be a JSON object with only a \"drives\" array"},
           Case{.text = "{", .message = "drive-specs.json: is not valid JSON"},
           Case{.text = R"({"drives": [{"model": "A", "source": "s", "tbw": 1}]})",
                .message = "drive-specs.json: drive #1: field 'tbw' is not a known field"},
           Case{.text = R"({"drives": [{"model": "A"}]})",
                .message =
                    "drive-specs.json: drive #1 'A': field 'source' must be a non-empty string"},
           Case{.text = R"({"drives": [{"model": "A", "source": "s", "rated_tbw": -5}]})",
                .message =
                    "drive-specs.json: drive #1 'A': field 'rated_tbw' must be a positive number"},
           Case{.text =
                    R"({"drives": [{"model": "A", "source": "s"}, {"model": "A", "source": "t"}]})",
                .message = "drive-specs.json: drive #2 'A': model is listed twice"},
       }) {
    const Result<DriveSpecs> specs = parse(bad.text);
    ASSERT_FALSE(specs.has_value()) << bad.text;
    EXPECT_EQ(specs.error().code, Errc::invalid_data);
    EXPECT_TRUE(specs.error().message.starts_with(bad.message)) << specs.error().message;
  }
}

TEST(DriveSpecs, MissingFileIsNotFound) {
  const rules::DiskDataFiles files;
  const Result<DriveSpecs> specs =
      DriveSpecs::load(files, std::filesystem::path(MYCLEAN_HEALTH_DATA) / "absent");
  ASSERT_FALSE(specs.has_value());
  EXPECT_EQ(specs.error().code, Errc::not_found);
}

}  // namespace
}  // namespace myclean::health
