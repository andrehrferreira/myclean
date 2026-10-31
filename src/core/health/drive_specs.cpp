#include "core/health/drive_specs.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/rules/data_files.hpp"

namespace myclean::health {

namespace {

using Json = nlohmann::json;

Result<std::string> required_string(const Json& entry, std::string_view key,
                                    const std::string& where) {
  const auto member = entry.find(key);
  if (member == entry.end() || !member->is_string() ||
      member->get_ref<const std::string&>().empty()) {
    return fail(Errc::invalid_data,
                std::format("{}: field '{}' must be a non-empty string", where, key));
  }
  return member->get<std::string>();
}

// A positive number, or nullopt when the field is absent.
Result<std::optional<double>> positive_number(const Json& entry, std::string_view key,
                                              const std::string& where) {
  const auto member = entry.find(key);
  if (member == entry.end()) {
    return std::optional<double>{};
  }
  if (!member->is_number() || member->get<double>() <= 0.0) {
    return fail(Errc::invalid_data,
                std::format("{}: field '{}' must be a positive number", where, key));
  }
  return std::optional<double>{member->get<double>()};
}

Result<DriveSpec> parse_entry(const Json& entry, const std::string& where) {
  if (!entry.is_object()) {
    return fail(Errc::invalid_data, where + ": must be a JSON object");
  }
  for (const auto& [key, value] : entry.items()) {
    if (key != "model" && key != "rated_tbw" && key != "max_temperature_c" && key != "source") {
      return fail(Errc::invalid_data,
                  std::format("{}: field '{}' is not a known field", where, key));
    }
  }
  Result<std::string> model = required_string(entry, "model", where);
  if (!model) {
    return tl::unexpected(model.error());
  }
  const std::string named = std::format("{} '{}'", where, *model);
  Result<std::string> source = required_string(entry, "source", named);
  Result<std::optional<double>> tbw = positive_number(entry, "rated_tbw", named);
  Result<std::optional<double>> temperature = positive_number(entry, "max_temperature_c", named);
  for (const Error* error : {source ? nullptr : &source.error(), tbw ? nullptr : &tbw.error(),
                             temperature ? nullptr : &temperature.error()}) {
    if (error != nullptr) {
      return tl::unexpected(*error);
    }
  }
  DriveSpec spec{.model = std::move(*model), .rated_tbw = *tbw, .source = std::move(*source)};
  if (*temperature) {
    spec.max_temperature_c = static_cast<std::int64_t>(**temperature);
  }
  return spec;
}

}  // namespace

Result<DriveSpecs> DriveSpecs::parse(std::string_view text, const std::filesystem::path& file) {
  const std::u8string utf8 = file.u8string();
  const std::string name(utf8.begin(), utf8.end());
  Json root;
  try {
    root = Json::parse(text);
  } catch (const Json::parse_error& error) {
    return fail(Errc::invalid_data, std::format("{}: is not valid JSON: {}", name, error.what()));
  }
  const auto drives = root.is_object() ? root.find("drives") : root.end();
  if (!root.is_object() || root.size() != 1 || drives == root.end() || !drives->is_array()) {
    return fail(Errc::invalid_data, name + ": must be a JSON object with only a \"drives\" array");
  }
  DriveSpecs specs;
  for (const Json& entry : *drives) {
    const std::string where = std::format("{}: drive #{}", name, specs.entries_.size() + 1);
    Result<DriveSpec> spec = parse_entry(entry, where);
    if (!spec) {
      return tl::unexpected(spec.error());
    }
    if (specs.find(spec->model) != nullptr) {
      return fail(Errc::invalid_data,
                  std::format("{} '{}': model is listed twice", where, spec->model));
    }
    specs.entries_.push_back(std::move(*spec));
  }
  return specs;
}

Result<DriveSpecs> DriveSpecs::load(const rules::IDataFiles& files,
                                    const std::filesystem::path& data_dir) {
  const std::filesystem::path file = data_dir / file_name;
  return files.read(file).and_then([&](const std::string& text) { return parse(text, file); });
}

const DriveSpec* DriveSpecs::find(std::string_view model) const {
  const auto found =
      std::ranges::find_if(entries_, [&](const DriveSpec& spec) { return spec.model == model; });
  return found == entries_.end() ? nullptr : &*found;
}

}  // namespace myclean::health
