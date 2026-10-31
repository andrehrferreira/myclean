#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/packs/schema.hpp"

// Pack fixtures (docs/packs.md, "Testing a case"): <pack>\tests\*.toml describe a fake machine
// and what each case must and must not select on it.
//
//   [env]                                   # optional: on top of the reference owner's
//   VCPKG_ROOT = 'D:\vcpkg'                 # environment (C:\Users\owner, C:\Windows, ...)
//
//   [workspace]                             # optional: roots the artifact scanner walks, for
//   roots = ['F:\']                         # clean cases with a project marker
//
//   [[entry]]                               # the fake file tree; parent folders are implied
//   path        = 'C:\Users\owner\AppData\Local\zig\o\1f\main.obj'
//   size        = 4096                      # optional, bytes of a file (default 4096)
//   age_days    = 365                       # optional, days since it changed (default 365)
//   folder      = true                      # optional: an empty folder instead of a file
//   git_ignored = true                      # optional: git ignores it
//
//   [[expect]]
//   kind     = "clean"                      # clean | attribution | relocate | compress
//   case     = "global-cache"               # the case id within the pack
//   match    = ['C:\Users\owner\AppData\Local\zig\o']   # each must be selected
//   no_match = ['C:\Users\owner\AppData\Local\zig\lib'] # none may be selected
//
// Selected means: clean, a candidate of the cleaning rule engine for the case (delete and
// quarantine; evaluate() or, with a project marker, scan_artifacts() over every clean case of the
// pack), or an entry its targets measure (native, handler); attribution and relocate, a path one
// of its locations names (component by component, '*' and '?' within one); compress, a path
// inside a folder its paths resolve to on the tree and not excluded.
namespace myclean::packs::testing {

// The failures of one fixture: one message per unmet expectation, or the fixture's own error.
[[nodiscard]] std::vector<std::string> run_fixture(const Pack& pack, std::string_view text,
                                                   const std::filesystem::path& file);

// run_fixture() of every tests\*.toml of the pack's folder, read from disk. `count` receives
// how many fixtures there were.
[[nodiscard]] std::vector<std::string> run_fixtures(const Pack& pack, int& count);

}  // namespace myclean::packs::testing
