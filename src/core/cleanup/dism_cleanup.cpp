#include "core/cleanup/dism_cleanup.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/cleanup/command_runner.hpp"
#include "core/cleanup/support.hpp"
#include "core/model/action.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/safety/paths.hpp"

namespace myclean::cleanup {

namespace {

constexpr std::uint64_t dism_unit = 1024;
constexpr std::uint32_t reboot_required = 3010;    // ERROR_SUCCESS_REBOOT_REQUIRED
constexpr std::uint32_t elevation_required = 740;  // ERROR_ELEVATION_REQUIRED
constexpr std::uint32_t win32_access_denied = 5;   // ERROR_ACCESS_DENIED
constexpr std::uint32_t hr_access_denied = 0x80070005;
constexpr std::uint32_t hr_elevation_required = 0x800702E4;
constexpr std::uint32_t pending_operations = 0x800F0806;  // CBS_E_PENDING
constexpr std::uint32_t win32_cancelled = 1223;           // ERROR_CANCELLED
constexpr std::uint32_t hr_cancelled = 0x800704C7;

}  // namespace

Result<ComponentStore> parse_component_store(std::string_view output) {
  std::map<std::string, std::string, std::less<>> fields;
  for (const std::string_view line : lines(output)) {
    if (const std::size_t colon = line.find(" : "); colon != std::string_view::npos) {
      fields.emplace(trim(line.substr(0, colon)), trim(line.substr(colon + 3)));
    }
  }
  const auto field = [&](std::string_view key) -> Result<std::string> {
    const auto found = fields.find(key);
    if (found == fields.end()) {
      return fail(Errc::invalid_data, "DISM output has no \"" + std::string(key) + "\"");
    }
    return found->second;
  };
  const auto size = [&](std::string_view key) -> Result<Bytes> {
    return field(key).and_then([&](const std::string& text) -> Result<Bytes> {
      const std::optional<Bytes> parsed = parse_size(text, dism_unit);
      if (!parsed) {
        return fail(Errc::invalid_data, std::format("DISM {}: unexpected size \"{}\"", key, text));
      }
      return *parsed;
    });
  };

  ComponentStore store;
  for (const auto& [key, target] :
       {std::pair{"Actual Size of Component Store", &store.actual_size},
        std::pair{"Backups and Disabled Features", &store.backups_and_disabled_features},
        std::pair{"Cache and Temporary Data", &store.cache_and_temporary_data}}) {
    const Result<Bytes> parsed = size(key);
    if (!parsed) {
      return tl::unexpected(parsed.error());
    }
    *target = *parsed;
  }
  const Result<std::string> packages = field("Number of Reclaimable Packages");
  if (!packages) {
    return tl::unexpected(packages.error());
  }
  const char* const end = std::to_address(packages->cend());
  if (const auto [rest, error] = std::from_chars(packages->data(), end, store.reclaimable_packages);
      error != std::errc{} || rest != end) {
    return fail(Errc::invalid_data, "DISM reclaimable packages: unexpected \"" + *packages + "\"");
  }
  const Result<std::string> recommended = field("Component Store Cleanup Recommended");
  if (!recommended) {
    return tl::unexpected(recommended.error());
  }
  if (*recommended != "Yes" && *recommended != "No") {
    return fail(Errc::invalid_data,
                "DISM cleanup recommended: unexpected \"" + *recommended + "\"");
  }
  store.cleanup_recommended = *recommended == "Yes";
  return store;
}

Result<void> dism_status(std::uint32_t exit_code) {
  switch (exit_code) {
    case 0:
    case reboot_required:
      return {};
    case elevation_required:
    case win32_access_denied:
    case hr_access_denied:
    case hr_elevation_required:
      return fail(Errc::access_denied,
                  std::format("DISM needs administrator rights (exit code {})", exit_code));
    case pending_operations:
      return fail(Errc::in_use,
                  "DISM: servicing operations are pending; Windows must restart first "
                  "(exit code 0x800F0806)");
    case win32_cancelled:
    case hr_cancelled:
      return fail(Errc::refused, std::format("DISM was cancelled (exit code {})", exit_code));
    default:
      return fail(Errc::io_error,
                  std::format("DISM failed with exit code {} (0x{:08X})", exit_code, exit_code));
  }
}

std::filesystem::path component_store_item() {
  return {"dism:component-store"};
}

DismCleanup::DismCleanup(ICommandRunner& runner, std::filesystem::path dism, Tier tier,
                         std::stop_token stop)
    : runner_(&runner), dism_(std::move(dism)), tier_(tier), stop_(std::move(stop)) {}

ActionInfo DismCleanup::describe() const {
  return {.what = "Remove superseded Windows components (DISM StartComponentCleanup)",
          .why = "Windows keeps replaced component versions in the component store",
          .risk = Risk::low,
          .tier = tier_,
          .context = ExecutionContext::service};
}

Result<ComponentStore> DismCleanup::analyze() const {
  const Result<CommandOutput> output = runner_->run(
      Command{.program = safety::to_utf8(dism_),
              .arguments = {"/Online", "/Cleanup-Image", "/AnalyzeComponentStore", "/English"},
              .timeout = analyze_timeout},
      stop_);
  if (!output) {
    return tl::unexpected(output.error());
  }
  if (const Result<void> status = dism_status(output->exit_code); !status) {
    return tl::unexpected(status.error());
  }
  return parse_component_store(output->out);
}

Result<Preview> DismCleanup::preview() {
  const Result<ComponentStore> store = analyze();
  if (!store) {
    return tl::unexpected(store.error());
  }
  Preview preview;
  if (store->cleanup_recommended) {
    preview.items.push_back(PlanItem{.path = component_store_item(), .size = store->overhead()});
  }
  return preview;
}

Result<Undo> DismCleanup::apply(const Preview& preview) {
  Undo undo{.not_reversible = "superseded components are removed"};
  before_ = preview.total_size();
  if (preview.items.empty()) {
    return undo;
  }
  const Result<CommandOutput> output = runner_->run(
      Command{.program = safety::to_utf8(dism_),
              .arguments = {"/Online", "/Cleanup-Image", "/StartComponentCleanup", "/English"},
              .timeout = cleanup_timeout},
      stop_);
  if (!output) {
    return tl::unexpected(output.error());
  }
  if (const Result<void> status = dism_status(output->exit_code); !status) {
    return tl::unexpected(status.error());
  }
  for (const PlanItem& item : preview.items) {
    undo.items.push_back({.item = item.id, .data = "dism"});
  }
  return undo;
}

Result<Verification> DismCleanup::verify(const Undo& /*undo*/) {
  const Result<ComponentStore> store = analyze();
  if (!store) {
    return tl::unexpected(store.error());
  }
  return compare("component_store_overhead_bytes", before_, store->overhead());
}

Result<void> DismCleanup::undo(const Undo& undo) {
  return fail(Errc::not_reversible, "not reversible: " + undo.not_reversible);
}

}  // namespace myclean::cleanup
