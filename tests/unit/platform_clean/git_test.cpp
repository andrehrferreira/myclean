#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <wil/resource.h>
#include <windows.h>

#include "core/clean/engine.hpp"
#include "core/clean/probes.hpp"
#include "core/clean/workspaces.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/rules/data_files.hpp"
#include "core/rules/schema.hpp"
#include "core/safety/deny_list.hpp"
#include "platform/clean/win32_probes.hpp"
#include "platform/fs/protected_locations.hpp"
#include "platform/git/git_cli.hpp"
#include "sandbox.hpp"
#include "shipped_rules.hpp"

namespace myclean::platform {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

fs::path command_processor() {
  std::wstring system(MAX_PATH, L'\0');
  system.resize(::GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size())));
  return fs::path{system} / L"cmd.exe";
}

// A restricted copy of this process's token (with nothing restricted): a token run_hidden may
// run a process as without the privileges of LocalSystem.
wil::unique_handle own_token() {
  wil::unique_handle own;
  wil::unique_handle copy;
  if (::OpenProcessToken(::GetCurrentProcess(),
                         TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY,
                         own.put()) == FALSE ||
      ::CreateRestrictedToken(own.get(), 0, 0, nullptr, 0, nullptr, 0, nullptr, copy.put()) ==
          FALSE) {
    return {};
  }
  return copy;
}

// This process's environment block with `extra` (NAME=value) added.
std::wstring environment_with(const std::wstring& extra) {
  const wil::unique_environstrings_ptr strings(::GetEnvironmentStringsW());
  std::wstring block;
  for (const wchar_t* entry = strings.get(); *entry != L'\0';) {
    const std::wstring_view text(entry);
    block.append(text).push_back(L'\0');
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): environment block
    entry += text.size() + 1;
  }
  block.append(extra).push_back(L'\0');
  block.push_back(L'\0');
  return block;
}

TEST(RunHidden, CapturesOutputAndExitCode) {
  const fs::path cmd = command_processor();
  const std::vector<std::wstring> arguments = {L"/d", L"/c", L"echo hello& exit 3"};

  const Result<ProcessOutput> output = run_hidden(cmd, arguments, 30s);

  ASSERT_TRUE(output.has_value()) << output.error().message;
  EXPECT_EQ(output->exit_code, 3U);
  EXPECT_EQ(output->out, "hello\r\n");
}

TEST(RunHidden, RunsAsTheGivenTokenWithItsEnvironment) {
  const ProcessIdentity identity{.token = own_token(),
                                 .environment = environment_with(L"MYCLEAN_WHO=owner")};
  ASSERT_TRUE(identity.token);
  const std::vector<std::wstring> arguments = {L"/d", L"/c", L"echo %MYCLEAN_WHO%"};

  const Result<ProcessOutput> output = run_hidden(command_processor(), arguments, 30s, &identity);

  ASSERT_TRUE(output.has_value()) << output.error().message;
  EXPECT_EQ(output->exit_code, 0U);
  EXPECT_EQ(output->out, "owner\r\n");
}

TEST(RunHidden, KillsAProgramThatOutlivesTheTimeout) {
  const std::vector<std::wstring> arguments = {L"/d", L"/c", L"ping -n 30 127.0.0.1 >NUL"};

  const auto started = std::chrono::steady_clock::now();
  const Result<ProcessOutput> output = run_hidden(command_processor(), arguments, 500ms);

  ASSERT_FALSE(output.has_value());
  EXPECT_EQ(output.error().code, Errc::io_error);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 20s);
}

// A git repository in a sandbox: package.json, src, an ignored node_modules, a dist matched by
// .gitignore that holds a force-added tracked file, and an unignored build folder.
class SandboxRepository : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::optional<fs::path> found = find_git();
    if (!found) {
      GTEST_SKIP() << "git is not on PATH";
    }
    git_path_ = *found;
    project_ = sandbox_.root() / "app";
    write_text(project_ / "package.json", "{}");
    write_text(project_ / ".gitignore", "node_modules/\ndist/\n");
    write_text(project_ / "src" / "index.js", "export {};\n");
    write_file(project_ / "node_modules" / "dep" / "index.js", 4096);
    write_file(project_ / "dist" / "keep.js", 4096);
    write_file(project_ / "build" / "out.js", 4096);
    fs::create_directories(sandbox_.root() / "no-hooks");
    git({L"init", L"-q"});
    git({L"add", L"package.json", L".gitignore", L"src"});
    git({L"add", L"-f", L"dist/keep.js"});
    git({L"commit", L"-q", L"-m", L"initial"});
  }

  static void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
  }

  // Runs a setup command in the project, isolated from the owner's hooks and signing.
  void git(std::vector<std::wstring> arguments) const { git_at(project_, std::move(arguments)); }

  // The same in `folder` (a worktree of the project).
  void git_at(const fs::path& folder, std::vector<std::wstring> arguments) const {
    std::vector<std::wstring> full = {
        L"-c", L"user.name=MyClean Test",
        L"-c", L"user.email=test@example.invalid",
        L"-c", L"commit.gpgsign=false",
        L"-c", L"core.hooksPath=" + (sandbox_.root() / "no-hooks").native(),
        L"-C", folder.native()};
    full.insert(full.end(), arguments.begin(), arguments.end());
    const Result<ProcessOutput> output = run_hidden(git_path_, full, 60s);
    ASSERT_TRUE(output.has_value()) << output.error().message;
    ASSERT_EQ(output->exit_code, 0U);
  }

  [[nodiscard]] const Sandbox& sandbox() const { return sandbox_; }
  [[nodiscard]] const fs::path& git_path() const { return git_path_; }
  [[nodiscard]] const fs::path& project() const { return project_; }

 private:
  Sandbox sandbox_;
  fs::path git_path_;
  fs::path project_;
};

TEST_F(SandboxRepository, AnswersIgnoredTrackedAndLastCommit) {
  const GitCli git{git_path()};

  const Result<std::optional<WallTime>> commit = git.last_commit(project());
  ASSERT_TRUE(commit.has_value()) << commit.error().message;
  ASSERT_TRUE(commit->has_value());
  EXPECT_LT(std::chrono::system_clock::now() - **commit, 10min);

  EXPECT_EQ(git.ignored(project(), project() / "node_modules"), true);
  // git does not report a folder that holds a tracked file as ignored, even when a pattern
  // matches it; tracks_files() answers for such a folder too.
  EXPECT_EQ(git.ignored(project(), project() / "dist"), false);
  EXPECT_EQ(git.ignored(project(), project() / "build"), false);
  EXPECT_EQ(git.ignored(project(), project() / "src"), false);
  EXPECT_EQ(git.tracks_files(project(), project() / "node_modules"), false);
  EXPECT_EQ(git.tracks_files(project(), project() / "dist"), true);
  EXPECT_FALSE(git.ignored(project(), sandbox().root() / "elsewhere").has_value());
}

// The service runs git as the owner: each command takes the identity its source gives, and
// one the source cannot give fails, so nothing is proposed from that repository.
TEST_F(SandboxRepository, RunsEveryCommandAsTheIdentityItsSourceGives) {
  int asked = 0;
  const GitCli git{git_path(), [&]() -> Result<ProcessIdentity> {
                     ++asked;
                     return ProcessIdentity{.token = own_token(),
                                            .environment = environment_with(L"MYCLEAN_WHO=owner")};
                   }};
  EXPECT_EQ(git.ignored(project(), project() / "node_modules"), true);
  EXPECT_EQ(git.tracks_files(project(), project() / "dist"), true);
  EXPECT_EQ(asked, 2);

  const GitCli logged_off{git_path(), []() -> Result<ProcessIdentity> {
                            return fail(Errc::not_found, "the owner is not logged on");
                          }};
  const Result<bool> ignored = logged_off.ignored(project(), project() / "node_modules");
  ASSERT_FALSE(ignored.has_value());
  EXPECT_EQ(ignored.error().code, Errc::not_found);
}

TEST_F(SandboxRepository, RepositoryWithoutCommitsHasNoLastCommit) {
  const fs::path empty = sandbox().root() / "empty";
  fs::create_directories(empty);
  const std::vector<std::wstring> init = {L"-C", empty.native(), L"init", L"-q"};
  ASSERT_EQ(run_hidden(git_path(), init, 60s)->exit_code, 0U);

  const Result<std::optional<WallTime>> commit = GitCli{git_path()}.last_commit(empty);
  ASSERT_TRUE(commit.has_value()) << commit.error().message;
  EXPECT_FALSE(commit->has_value());
  EXPECT_FALSE(GitCli{git_path()}.last_commit(sandbox().root() / "no-repo").has_value());
}

// The linked worktree git describes at `folder`; the test fails when git cannot answer or
// describes none.
clean::Worktree described(const GitCli& git, const fs::path& folder) {
  const Result<std::optional<clean::Worktree>> worktree = git.worktree(folder);
  EXPECT_TRUE(worktree.has_value() && worktree->has_value())
      << (worktree ? "not a linked worktree" : worktree.error().message);
  return worktree.value_or(std::nullopt).value_or(clean::Worktree{});
}

TEST_F(SandboxRepository, DescribesLinkedWorktrees) {
  git({L"branch", L"-M", L"main"});
  const fs::path merged = sandbox().root() / "merged";
  const fs::path detached = sandbox().root() / "detached";
  const fs::path ahead = sandbox().root() / "ahead";
  const fs::path dirty = sandbox().root() / "dirty";
  git({L"worktree", L"add", L"-q", L"-b", L"done", merged.native()});
  git({L"worktree", L"add", L"-q", L"--detach", detached.native()});
  git({L"worktree", L"add", L"-q", L"-b", L"ahead", ahead.native()});
  git_at(ahead, {L"commit", L"-q", L"--allow-empty", L"-m", L"not merged"});
  git({L"worktree", L"add", L"-q", L"-b", L"dirty", dirty.native()});
  write_text(dirty / "new.txt", "untracked");
  const GitCli git{git_path()};

  const clean::Worktree done = described(git, merged);
  EXPECT_TRUE(fs::equivalent(done.repository, project() / ".git"));
  EXPECT_EQ(done.branch, "done");
  EXPECT_EQ(done.base, "main");
  EXPECT_TRUE(done.merged);
  EXPECT_TRUE(done.clean);
  const clean::Worktree head = described(git, detached);
  EXPECT_EQ(head.branch, "");
  EXPECT_TRUE(head.merged && head.clean);
  EXPECT_FALSE(described(git, ahead).merged);
  const clean::Worktree changed = described(git, dirty);
  EXPECT_TRUE(changed.merged);
  EXPECT_FALSE(changed.clean);
  // The repository's own work tree is not a linked worktree; one whose gitdir is gone fails.
  const Result<std::optional<clean::Worktree>> own = git.worktree(project());
  ASSERT_TRUE(own.has_value()) << own.error().message;
  EXPECT_FALSE(own->has_value());
  write_text(sandbox().root() / "stale" / ".git",
             "gitdir: " + sandbox().root().string() + "/gone\n");
  EXPECT_FALSE(git.worktree(sandbox().root() / "stale").has_value());
}

TEST_F(SandboxRepository, ComparesWithTheBranchOfOriginHead) {
  const fs::path merged = sandbox().root() / "merged";
  git({L"worktree", L"add", L"-q", L"-b", L"done", merged.native()});
  git({L"update-ref", L"refs/remotes/origin/trunk", L"HEAD"});
  git({L"symbolic-ref", L"refs/remotes/origin/HEAD", L"refs/remotes/origin/trunk"});

  const clean::Worktree done = described(GitCli{git_path()}, merged);

  EXPECT_EQ(done.base, "origin/trunk");
  EXPECT_TRUE(done.merged && done.clean);
}
// The developer-artifact scan end to end on the real file system and git, with the shipped
// rules, two months from now: only the ignored, untracked node_modules is proposed.
TEST_F(SandboxRepository, ScanProposesOnlyIgnoredUntrackedArtifacts) {
  const std::vector<rules::CleaningRule> shipped = testing::shipped_rules();
  ASSERT_FALSE(shipped.empty());
  const Result<safety::ProtectedLocations> locations = protected_locations(nullptr);
  ASSERT_TRUE(locations.has_value()) << locations.error().message;
  const Win32FileTree tree;
  const Win32Environment environment;
  const Win32Usage usage;
  const GitCli git{git_path()};
  const clean::RuleEngine engine{tree, environment, usage, *locations};
  const clean::Workspaces workspaces{
      .roots = {sandbox().root()}, .skip = {L".git", L"node_modules"}, .max_depth = 4};

  const clean::Evaluation evaluation = engine.scan_artifacts(
      shipped, workspaces, git, std::chrono::system_clock::now() + std::chrono::days{60});

  ASSERT_EQ(evaluation.candidates.size(), 1U);
  const clean::Candidate& candidate = evaluation.candidates.front();
  EXPECT_EQ(candidate.path, project() / "node_modules");
  EXPECT_EQ(candidate.rule_id, "node/artifacts");
  EXPECT_EQ(candidate.project, project());
  EXPECT_GE(candidate.size, Bytes{4096});
  EXPECT_EQ(git.tracks_files(project(), candidate.path), false);
  for (const clean::Dropped& dropped : evaluation.dropped) {
    if (dropped.path == project() / "dist" || dropped.path == project() / "build") {
      EXPECT_EQ(dropped.reason, "not_ignored");
    } else {
      ADD_FAILURE() << "unexpected drop of " << dropped.path << ": " << dropped.reason;
    }
  }
  EXPECT_EQ(evaluation.dropped.size(), 2U);
}

TEST_F(SandboxRepository, RecentProjectProposesNothing) {
  const std::vector<rules::CleaningRule> shipped = testing::shipped_rules();
  ASSERT_FALSE(shipped.empty());
  const Win32FileTree tree;
  const Win32Environment environment;
  const Win32Usage usage;
  const GitCli git{git_path()};
  const clean::RuleEngine engine{tree, environment, usage, *protected_locations(nullptr)};
  const clean::Workspaces workspaces{.roots = {sandbox().root()}, .skip = {}, .max_depth = 4};

  const clean::Evaluation evaluation =
      engine.scan_artifacts(shipped, workspaces, git, std::chrono::system_clock::now());

  EXPECT_TRUE(evaluation.candidates.empty());
  EXPECT_TRUE(evaluation.dropped.empty());
}

}  // namespace
}  // namespace myclean::platform
