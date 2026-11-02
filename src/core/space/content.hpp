#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "core/model/result.hpp"

namespace myclean::space {

// Reads byte ranges of file content: the member lists of archives (scan-all). Implemented by
// src/platform/scan.
class IContentReader {
 public:
  virtual ~IContentReader() = default;
  IContentReader(const IContentReader&) = delete;
  IContentReader& operator=(const IContentReader&) = delete;
  IContentReader(IContentReader&&) = delete;
  IContentReader& operator=(IContentReader&&) = delete;

  // Up to `length` bytes of `file` from `offset`: fewer at the end of the file, none past it.
  // Fails with refused for a link or a cloud placeholder, whose content is never read.
  [[nodiscard]] virtual Result<std::string> read(const std::filesystem::path& file,
                                                 std::uint64_t offset,
                                                 std::size_t length) const = 0;

 protected:
  IContentReader() = default;
};

}  // namespace myclean::space
