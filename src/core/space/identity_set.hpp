#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_set>

#include "core/model/file_identity.hpp"

namespace myclean::space {

// The file identities seen so far; hard links of one file share an identity. An identity that
// is all zero (a file system without file ids) is never reported as seen.
class IdentitySet {
 public:
  // True when `identity` was not seen before (or is unknown).
  bool insert(const FileIdentity& identity) {
    if (identity == FileIdentity{}) {
      return true;
    }
    return seen_.insert(identity).second;
  }

 private:
  struct Hash {
    std::size_t operator()(const FileIdentity& identity) const {
      std::size_t hash = std::hash<std::uint64_t>{}(identity.volume_serial);
      for (const std::uint8_t byte : identity.file_id) {
        hash = (hash * 131U) ^ byte;
      }
      return hash;
    }
  };

  std::unordered_set<FileIdentity, Hash> seen_;
};

}  // namespace myclean::space
