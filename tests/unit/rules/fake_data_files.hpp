#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/rules/data_files.hpp"

namespace myclean::rules {

// In-memory IDataFiles: a directory exists once a file was put into it.
class FakeDataFiles final : public IDataFiles {
 public:
  void put(const std::filesystem::path& file, std::string text) { files_[file] = std::move(text); }
  void erase(const std::filesystem::path& file) { files_.erase(file); }

  [[nodiscard]] Result<std::vector<std::filesystem::path>> list(
      const std::filesystem::path& dir) const override {
    std::vector<std::filesystem::path> listed;
    for (const auto& [file, text] : files_) {
      if (file.parent_path() == dir) {
        listed.push_back(file);
      }
    }
    if (listed.empty()) {
      return fail(Errc::not_found, "no such directory");
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

 private:
  std::map<std::filesystem::path, std::string> files_;
};

}  // namespace myclean::rules
