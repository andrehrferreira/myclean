#pragma once

#include <filesystem>
#include <functional>
#include <vector>

#include "core/clean/probes.hpp"
#include "core/model/units.hpp"

// Finding and measuring entries through IFileTree, never through a reparse point.
namespace myclean::clean {

struct Matches {
  std::vector<Entry> entries;
  // Reparse points the pattern would have had to pass through; nothing below them is matched.
  std::vector<std::filesystem::path> links;
};

// The entries matching `pattern` below directory `base`, which the caller reached without
// passing a link. Every component of `pattern` may hold '*' and '?'; the last one may match
// files, folders and links, the others only folders that are neither links nor cloud
// placeholders. A folder that cannot be listed matches nothing; a ".." component matches
// nothing at all.
[[nodiscard]] Matches resolve(const IFileTree& tree, const std::filesystem::path& base,
                              const std::filesystem::path& pattern);

// True when `pattern` starts with "?:\": the same path on every fixed volume.
[[nodiscard]] bool on_every_volume(const std::filesystem::path& pattern);

// resolve() of an absolute pattern from its volume root; a pattern on_every_volume() is resolved
// from the root of each of the tree's fixed_volumes().
[[nodiscard]] Matches resolve(const IFileTree& tree, const std::filesystem::path& pattern);

enum class Visit {
  descend,  // go on; enter it when it is a folder
  skip,     // go on, but do not enter this folder
  stop,     // end the walk
};

enum class WalkEnd {
  complete,
  stopped,     // a visit returned stop
  incomplete,  // a folder could not be listed (one that vanished meanwhile does not count)
};

// Visits every entry below folder `root`, depth first, each folder before its contents.
// Reparse points and cloud placeholders are visited but never entered.
WalkEnd walk(const IFileTree& tree, const std::filesystem::path& root,
             const std::function<Visit(const Entry&)>& visit);

struct Measure {
  Bytes size;       // size on disk of the entry and everything below it
  WallTime newest;  // the newest `touched` of the entry and everything below it
  bool complete = true;
};

[[nodiscard]] Measure measure(const IFileTree& tree, const Entry& entry);

}  // namespace myclean::clean
