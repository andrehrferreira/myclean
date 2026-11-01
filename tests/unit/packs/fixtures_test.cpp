#include "core/packs/fixtures.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/packs/loader.hpp"
#include "core/packs/schema.hpp"
#include "pack_fakes.hpp"

// The fixture runner on a small pack: what each kind selects, and how failures are reported.
namespace myclean::packs::testing {
namespace {

namespace fs = std::filesystem;

const fs::path folder = R"(C:\packs\tool)";
const fs::path fixture = folder / "tests" / "machine.toml";

Pack tool_pack() {
  FakePackFiles files;
  files.put(folder / "pack.toml", R"(id = "tool"
version = "1.0.0"
title = "Tool"
description = "A tool."
sources = ["observed on reference machine"]
min_myclean = "0.1.0"
)");
  files.put(folder / "clean.toml", R"([[rule]]
id       = "cache"
category = "dev-caches"
tier     = "ask"
scope    = "user"
strategy = "delete"
targets  = ['%LOCALAPPDATA%\tool\cache\*']
require  = { min_age_days = 30 }

[[rule]]
id       = "prune"
category = "dev-caches"
tier     = "ask"
scope    = "user"
strategy = "native"
command  = "pnpm store prune"
targets  = ['%LOCALAPPDATA%\tool\store']

[[rule]]
id       = "build"
category = "dev-artifacts"
tier     = "ask"
scope    = "user"
strategy = "delete"
project  = { marker = ".tool-root" }
targets  = ["out"]
require  = { git_ignored = true, inactive_days = 30 }

[[rule]]
id       = "leftovers"
category = "dev-caches"
tier     = "ask"
scope    = "user"
strategy = "delete"
targets  = ['?:\tool\leftovers\*']
require  = { min_age_days = 7 }
)");
  files.put(folder / "attribution.toml", R"([[area]]
id       = "home"
paths    = ['?:\tool\*']
category = "dev-caches"
label    = "Tool homes"
reclaim  = { kind = "none" }
)");
  files.put(folder / "relocate.toml", R"([[relocate]]
id          = "home"
label       = "Tool home"
locations   = ['%TOOL_HOME%', '%USERPROFILE%\.tool']
current     = { env = "TOOL_HOME" }
target_kind = "env"
content     = "move"
undo        = "restore"
)");
  files.put(folder / "compress.toml", R"([[compress]]
id        = "bin"
label     = "Tool binaries"
paths     = ['%USERPROFILE%\.tool\versions\*']
algorithm = "lzx"
exclude   = ["*.log", "tmp"]
)");
  files.put(folder / "kb.json", R"({"entries": [{"id": "agent", "image": "ToolAgent.exe",
    "description": "Keeps the tool up to date.", "category": "updater",
    "needed_at_startup": false, "safe_to_remove": true}]})");
  Result<Pack> pack =
      load_pack(files, folder, Origin::builtin, Version{.major = 0, .minor = 1, .patch = 0});
  if (!pack) {
    throw std::logic_error(pack.error().message);
  }
  return std::move(*pack);
}

std::vector<std::string> run(std::string_view text) {
  return run_fixture(tool_pack(), text, fixture);
}

TEST(Fixtures, CleanCasesSelectWhatTheEngineProposes) {
  const std::vector<std::string> failures = run(R"(
[[entry]]
path = 'C:\Users\owner\AppData\Local\tool\cache\old\a.bin'
[[entry]]
path = 'C:\Users\owner\AppData\Local\tool\cache\new\a.bin'
age_days = 1
[[entry]]
path = 'C:\Users\owner\AppData\Local\tool\store\blob'

[[expect]]
kind = "clean"
case = "cache"
match = ['C:\Users\owner\AppData\Local\tool\cache\old']
no_match = ['C:\Users\owner\AppData\Local\tool\cache\new', 'C:\Users\owner\AppData\Local\tool\cache']

[[expect]]
kind = "clean"
case = "prune"
match = ['C:\Users\owner\AppData\Local\tool\store']
)");
  EXPECT_TRUE(failures.empty()) << failures.front();
}

// "?:\" stands for the fixture's fixed volumes: those it declares, or else every drive of its
// entries.
TEST(Fixtures, AnyDriveTargetsCoverTheFixedVolumes) {
  constexpr std::string_view entries = R"(
[[entry]]
path = 'C:\tool\leftovers\a\x'
[[entry]]
path = 'F:\tool\leftovers\b\x'
[[entry]]
path = 'H:\tool\leftovers\c\x'
)";
  const std::vector<std::string> every_drive = run(std::string(entries) + R"(
[[expect]]
kind = "clean"
case = "leftovers"
match = ['C:\tool\leftovers\a', 'F:\tool\leftovers\b', 'H:\tool\leftovers\c']
)");
  EXPECT_TRUE(every_drive.empty()) << every_drive.front();
  const std::vector<std::string> declared =
      run(R"(volumes = ['C:\', 'F:\'])" + std::string(entries) + R"(
[[expect]]
kind = "clean"
case = "leftovers"
match = ['C:\tool\leftovers\a', 'F:\tool\leftovers\b']
no_match = ['H:\tool\leftovers\c']
)");
  EXPECT_TRUE(declared.empty()) << declared.front();
  EXPECT_NE(run(R"(volumes = ['F:\tool']
[[expect]]
kind = "kb"
case = "agent"
match = ['ToolAgent.exe']
)")
                .at(0)
                .find(R"(field 'volumes' must hold volume roots such as 'C:\', not 'F:\tool')"),
            std::string::npos);
}

TEST(Fixtures, KbCasesSelectTheirImageInAnyCase) {
  const std::vector<std::string> failures = run(R"(
[[expect]]
kind = "kb"
case = "agent"
match = ['toolagent.exe']
no_match = ['Tool.exe']
)");
  EXPECT_TRUE(failures.empty()) << failures.front();
  const std::vector<std::string> wrong = run(R"(
[[expect]]
kind = "kb"
case = "agent"
match = ['Tool.exe']
)");
  ASSERT_EQ(wrong.size(), 1U);
  EXPECT_NE(wrong.front().find("(kb 'tool/agent'): 'Tool.exe' is not selected"), std::string::npos)
      << wrong.front();
}

// A file's folders exist once the file does, so a folder entry must come before its files.
TEST(Fixtures, AFolderEntryAfterItsFilesIsAnError) {
  const std::vector<std::string> failures = run(R"(
[[entry]]
path = 'F:\src\out\x.o'
[[entry]]
path = 'F:\src\out'
folder = true
)");
  ASSERT_EQ(failures.size(), 1U);
  EXPECT_NE(failures[0].find("entry #2: field 'path' repeats an entry"), std::string::npos)
      << failures[0];
}

TEST(Fixtures, ProjectCasesScanTheWorkspaceAndNeedGit) {
  const std::vector<std::string> failures = run(R"(
[workspace]
roots = ['F:\']

[[entry]]
path = 'F:\src\.git'
folder = true
[[entry]]
path = 'F:\src\.tool-root'
[[entry]]
path = 'F:\src\out'
folder = true
git_ignored = true
[[entry]]
path = 'F:\src\out\x.o'
[[entry]]
path = 'F:\loose\.tool-root'
[[entry]]
path = 'F:\loose\out\x.o'

[[expect]]
kind = "clean"
case = "build"
match = ['F:\src\out']
no_match = ['F:\loose\out']
)");
  EXPECT_TRUE(failures.empty()) << failures.front();
}

TEST(Fixtures, LocationKindsMatchByPattern) {
  const std::vector<std::string> failures = run(R"(
[env]
TOOL_HOME = 'D:\tools\home'

[[entry]]
path = 'C:\Users\owner\.tool\versions\1.0\bin\tool.exe'
[[entry]]
path = 'C:\Users\owner\.tool\versions\1.0\tmp\x'

[[expect]]
kind = "attribution"
case = "home"
match = ['C:\tool\a', 'D:\tool\b']
no_match = ['C:\tool', 'C:\tool\a\b', 'C:\other\a']

[[expect]]
kind = "relocate"
case = "home"
match = ['D:\tools\home', 'C:\Users\owner\.tool']
no_match = ['C:\Users\owner\.tool\versions']

[[expect]]
kind = "compress"
case = "bin"
match = ['C:\Users\owner\.tool\versions\1.0\bin\tool.exe', 'C:\Users\owner\.tool\versions\1.0']
no_match = ['C:\Users\owner\.tool\versions\1.0\tmp\x', 'C:\Users\owner\.tool\versions\1.0\a.log',
            'C:\Users\owner\.tool\versions']
)");
  EXPECT_TRUE(failures.empty()) << failures.front();
}

TEST(Fixtures, ReportsEveryUnmetExpectation) {
  const std::vector<std::string> failures = run(R"(
[[entry]]
path = 'C:\Users\owner\AppData\Local\tool\cache\old\a.bin'

[[expect]]
kind = "clean"
case = "cache"
match = ['C:\Users\owner\AppData\Local\tool\cache\other']
no_match = ['C:\Users\owner\AppData\Local\tool\cache\old']
)");
  ASSERT_EQ(failures.size(), 2U);
  EXPECT_NE(failures[0].find("expect #1 (clean 'tool/cache'): "
                             R"('C:\Users\owner\AppData\Local\tool\cache\other' is not selected)"),
            std::string::npos)
      << failures[0];
  EXPECT_NE(failures[1].find("is selected"), std::string::npos) << failures[1];
}

TEST(Fixtures, RejectsInvalidFixtures) {
  EXPECT_NE(run("").at(0).find("field 'expect' must list at least one expectation"),
            std::string::npos);
  EXPECT_NE(run("[[expect]]\nkind = \"clean\"\ncase = \"missing\"\nmatch = ['C:\\x']\n")
                .at(0)
                .find("field 'case' names no clean case of the pack"),
            std::string::npos);
  EXPECT_NE(run("[[expect]]\nkind = \"classes\"\ncase = \"cache\"\nmatch = ['C:\\x']\n")
                .at(0)
                .find("field 'kind' must be one of clean, attribution, relocate, compress, kb"),
            std::string::npos);
  EXPECT_NE(run("[[expect]]\nkind = \"clean\"\ncase = \"cache\"\n")
                .at(0)
                .find("sets neither match nor no_match"),
            std::string::npos);
  EXPECT_NE(run("[[entry]]\npath = 'tool\\x'\n").at(0).find("field 'path' must be an absolute"),
            std::string::npos);
}

}  // namespace
}  // namespace myclean::packs::testing
