#pragma once

#include <array>
#include <cstdint>

namespace myclean {

// Identity of a file system entry: unchanged by a rename on the same volume, different after
// the entry is deleted and recreated. Recorded in a plan so that an item can be re-validated
// right before it is acted on.
struct FileIdentity {
  std::uint64_t volume_serial = 0;
  std::array<std::uint8_t, 16> file_id{};  // FILE_ID_128; 64-bit NTFS ids fill the low 8 bytes

  friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

}  // namespace myclean
