#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/text.hpp"
#include "core/model/units.hpp"
#include "core/vhdx/compact.hpp"
#include "fakes/fake_file_system.hpp"

namespace myclean::vhdx::testing {

// Records whether WSL was shut down, or fails it.
class FakeWsl final : public IWsl {
 public:
  int shutdowns = 0;
  std::optional<Error> error;
  std::vector<std::string>* journal = nullptr;  // "shutdown" is appended when set

  [[nodiscard]] Result<void> shutdown() override {
    if (journal != nullptr) {
      journal->emplace_back("shutdown");
    }
    if (error) {
      return tl::unexpected(*error);
    }
    ++shutdowns;
    return {};
  }
};

// Compacts a disk of `file_system` to the size `after` names for it (unchanged when absent), or
// fails with the error `errors` names for it.
class FakeVirtualDisks final : public IVirtualDisks {
 public:
  explicit FakeVirtualDisks(fakes::FakeFileSystem& disks_on) : file_system(&disks_on) {}

  fakes::FakeFileSystem* file_system;
  std::map<std::filesystem::path, Bytes> after;
  std::map<std::filesystem::path, Error> errors;
  std::vector<std::filesystem::path> compacted;
  std::vector<std::string>* journal = nullptr;  // "compact <name>" is appended when set

  [[nodiscard]] Result<void> compact(const std::filesystem::path& file) override {
    if (journal != nullptr) {
      journal->push_back("compact " + utf8_text(file.filename()));
    }
    if (const auto error = errors.find(file); error != errors.end()) {
      return tl::unexpected(error->second);
    }
    if (!file_system->exists(file)) {
      return fail(Errc::not_found, "no such disk");
    }
    if (const auto size = after.find(file); size != after.end()) {
      file_system->erase(file);
      file_system->add_file(file, size->second);
    }
    compacted.push_back(file);
    return {};
  }
};

}  // namespace myclean::vhdx::testing
