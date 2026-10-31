#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/packs/applies.hpp"
#include "core/packs/loader.hpp"

namespace myclean::packs::testing {

// In-memory IPackFiles: a folder exists once a file was put somewhere below it.
class FakePackFiles final : public IPackFiles {
 public:
  void put(const std::filesystem::path& file, std::string text) { files_[file] = std::move(text); }

  [[nodiscard]] Result<std::vector<std::filesystem::path>> list(
      const std::filesystem::path& dir) const override {
    if (!exists(dir)) {
      return fail(Errc::not_found, "no such folder");
    }
    std::vector<std::filesystem::path> listed;
    for (const auto& [file, text] : files_) {
      if (file.parent_path() == dir) {
        listed.push_back(file);
      }
    }
    return listed;
  }

  [[nodiscard]] Result<std::string> read(const std::filesystem::path& file) const override {
    const auto found = files_.find(file);
    if (found == files_.end()) {
      return fail(Errc::not_found, "no such file");
    }
    return found->second;
  }

  [[nodiscard]] Result<std::vector<std::filesystem::path>> folders(
      const std::filesystem::path& dir) const override {
    if (!exists(dir)) {
      return fail(Errc::not_found, "no such folder");
    }
    std::set<std::filesystem::path> found;
    for (const auto& [file, text] : files_) {
      const std::filesystem::path relative = file.lexically_relative(dir);
      if (std::distance(relative.begin(), relative.end()) > 1 && *relative.begin() != "..") {
        found.insert(dir / *relative.begin());
      }
    }
    return std::vector<std::filesystem::path>(found.begin(), found.end());
  }

 private:
  [[nodiscard]] bool exists(const std::filesystem::path& dir) const {
    return std::ranges::any_of(files_, [&dir](const auto& entry) {
      const std::filesystem::path relative = entry.first.lexically_relative(dir);
      return !relative.empty() && *relative.begin() != "..";
    });
  }

  std::map<std::filesystem::path, std::string> files_;
};

class FakeMachine final : public IMachine {
 public:
  std::vector<std::string> apps;
  std::vector<std::string> images;
  std::uint32_t build = 26100;

  [[nodiscard]] std::vector<std::string> installed_apps() const override { return apps; }
  [[nodiscard]] std::vector<std::string> running_images() const override { return images; }
  [[nodiscard]] std::uint32_t os_build() const override { return build; }
};

}  // namespace myclean::packs::testing
