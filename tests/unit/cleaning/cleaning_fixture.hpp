#pragma once

#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "actions/actions_test_support.hpp"
#include "clean/clean_fakes.hpp"
#include "cleanup/fakes.hpp"
#include "core/actions/audit_log.hpp"
#include "core/actions/schema.hpp"
#include "core/clean/probes.hpp"
#include "core/cleaning/records.hpp"
#include "core/cleaning/service.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/duplicates.hpp"
#include "core/startup/impact.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_file_system.hpp"
#include "packs/pack_fakes.hpp"
#include "placement/placement_fakes.hpp"
#include "space/space_fakes.hpp"
#include "startup/startup_fakes.hpp"

namespace myclean::cleaning::testing {

using Json = nlohmann::json;

// 2026-10-31 12:00:00 UTC.
inline const WallTime noon =
    std::chrono::sys_days{std::chrono::October / 31 / 2026} + std::chrono::hours{12};

// The cleaning engine's file tree over a FakeFileSystem, so that rule candidates and SafeRemove
// see the same entries. Every entry was last touched `touched`.
class FsTree final : public clean::IFileTree {
 public:
  FsTree(const fakes::FakeFileSystem& file_system, WallTime touched)
      : file_system_(&file_system), touched_(touched) {}

  [[nodiscard]] Result<clean::Entry> stat(const std::filesystem::path& path) const override {
    Result<os::FileStat> status = file_system_->stat(path);
    if (!status) {
      return tl::unexpected(status.error());
    }
    return clean::Entry{.path = path, .stat = *status, .touched = touched_};
  }
  [[nodiscard]] Result<std::vector<clean::Entry>> list(
      const std::filesystem::path& path) const override {
    Result<std::vector<os::DirEntry>> entries = file_system_->enumerate(path);
    if (!entries) {
      return tl::unexpected(entries.error());
    }
    std::vector<clean::Entry> listed;
    for (os::DirEntry& entry : *entries) {
      listed.push_back(
          clean::Entry{.path = std::move(entry.path), .stat = entry.stat, .touched = touched_});
    }
    return listed;
  }
  // Every drive root the fake file system holds.
  [[nodiscard]] std::vector<std::filesystem::path> fixed_volumes() const override {
    std::vector<std::filesystem::path> volumes;
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
      const std::filesystem::path root = std::wstring{letter, L':', L'\\'};
      if (file_system_->exists(root)) {
        volumes.push_back(root);
      }
    }
    return volumes;
  }

 private:
  const fakes::FakeFileSystem* file_system_;
  WallTime touched_;
};

// An owner context's git: answers from a FakeGit the fixture keeps.
class ForwardingGit final : public clean::IGit {
 public:
  explicit ForwardingGit(const clean::IGit& git) : git_(&git) {}

  [[nodiscard]] Result<std::optional<WallTime>> last_commit(
      const std::filesystem::path& work_tree) const override {
    return git_->last_commit(work_tree);
  }
  [[nodiscard]] Result<bool> ignored(const std::filesystem::path& work_tree,
                                     const std::filesystem::path& entry) const override {
    return git_->ignored(work_tree, entry);
  }
  [[nodiscard]] Result<bool> tracks_files(const std::filesystem::path& work_tree,
                                          const std::filesystem::path& entry) const override {
    return git_->tracks_files(work_tree, entry);
  }

 private:
  const clean::IGit* git_;
};

inline const std::filesystem::path data_dir = L"Z:\\data";

// The clean cases the tests evaluate, by tier and strategy: pack "test" of data_dir\packs.
inline constexpr std::string_view test_manifest = R"toml(id = "test"
version = "1.0.0"
title = "Test"
description = "The cleaning tests' cases."
sources = ["observed on reference machine"]
min_myclean = "0.1.0"
)toml";

inline constexpr std::string_view test_rules = R"toml(
[[rule]]
id       = "auto"
category = "temp"
tier     = "auto"
scope    = "user"
strategy = "delete"
targets  = ['F:\temp\*']

[[rule]]
id       = "ask.f"
category = "shader-caches"
tier     = "ask"
scope    = "user"
strategy = "quarantine"
targets  = ['F:\cache\*']

[[rule]]
id       = "ask.c"
category = "c-caches"
tier     = "ask"
scope    = "user"
strategy = "quarantine"
targets  = ['C:\cache\*']

[[rule]]
id       = "report"
category = "logs"
tier     = "report"
scope    = "user"
strategy = "delete"
targets  = ['F:\logs\*']

[[rule]]
id       = "native"
category = "package-managers"
tier     = "ask"
scope    = "user"
strategy = "native"
command  = "pnpm store prune"
targets  = ['F:\pnpm\store']

[[rule]]
id       = "handler"
category = "windows"
tier     = "ask"
scope    = "system"
strategy = "handler"
handler  = "Update Cleanup"
)toml";

inline void put_test_pack(packs::testing::FakePackFiles& files) {
  files.put(data_dir / "packs" / "test" / "pack.toml", std::string(test_manifest));
  files.put(data_dir / "packs" / "test" / "clean.toml", std::string(test_rules));
}

// Hashes a file by the digits of its name: "a17.bin" and "b17.bin" have the same content.
class NameDigitHasher final : public space::IContentHasher {
 public:
  [[nodiscard]] Result<space::Digest> hash(const std::filesystem::path& file,
                                           std::optional<Bytes> /*limit*/) const override {
    std::string digits;
    const std::wstring name = file.filename().native();
    for (const wchar_t character : name) {
      if (character >= L'0' && character <= L'9') {
        digits += static_cast<char>(character);
      }
    }
    space::Digest digest{};
    const std::size_t value = std::hash<std::string>{}(digits);
    std::memcpy(digest.data(), &value, sizeof(value));
    return digest;
  }
};

// A CleaningService over fakes, with its methods on a dispatcher. Volumes as on the owner's
// machine: C: has 8 % free (its quarantine overflows to L:), F: 13 %.
class CleaningFixture : public ::testing::Test {
 protected:
  CleaningFixture() {
    put_test_pack(files_);
    drives_.list() = placement::testing::owner_volumes();
    for (const placement::TargetVolume& volume : drives_.list()) {
      fs_.add_directory(volume.root / "data");
    }
    handlers_.sizes["Update Cleanup"] = Bytes{7ULL << 30U};
    // Already as small as automatic reclaiming makes them, although C: is low on space.
    system_.hibernation_state.mode = placement::HibernationMode::reduced;
    open_store();
  }

  // Starts the service (again: a restart over the same store).
  void start(ServiceSettings settings = {}) {
    service_.reset();
    methods_ = std::make_unique<ipc::Dispatcher>();
    ServiceSources sources{
        .store = store_.get(),
        .clock = &clock_,
        .file_system = &fs_,
        .probes = {.tree = &tree_,
                   .usage = &usage_,
                   .handlers = &handlers_,
                   .system = &system_,
                   .tools = &tools_,
                   .drives = &drives_,
                   .startup = &startup_},
        .owner = [this]() -> Result<OwnerContext> {
          if (!owner_available_) {
            return fail(Errc::not_found, "the owner is not logged on");
          }
          return OwnerContext{.environment = std::make_unique<clean::testing::FakeEnvironment>(),
                              .locations = actions::testing::owner_locations(),
                              .downloads = std::filesystem::path(L"C:\\Users\\owner\\Downloads"),
                              .git = owner_git_ ? std::make_unique<ForwardingGit>(git_) : nullptr,
                              .tools = owner_registry_
                                           ? std::shared_ptr<const placement::Tools>(
                                                 &owner_tools_, [](const placement::Tools*) {})
                                           : nullptr};
        },
        .files = &files_,
        .data_dir = data_dir,
        .space = {.reader = &reader_, .hasher = &hasher_},
        .busy = [this] { return busy_; },
        .diagnostics = &diagnostics_};
    service_ = std::make_unique<CleaningService>(std::move(sources), std::move(settings));
    ASSERT_TRUE(service_->start(*methods_).has_value());
  }

  void work() { service_->work({}); }
  void work_space() { service_->work_space({}); }

  // The result of `method`, failing the test on an error.
  Json call(std::string_view method, const Json& params = Json::object(), bool tray = true) {
    Result<Json> result = try_call(method, params, tray);
    EXPECT_TRUE(result.has_value()) << method << ": " << result.error().message;
    return result.value_or(Json());
  }
  Result<Json> try_call(std::string_view method, const Json& params = Json::object(),
                        bool tray = true) {
    return ipc::parse_response(
        methods_->handle(ipc::make_request(1, method, params), ipc::Caller{.tray = tray}), 1);
  }
  // The JSON-RPC error of a call: {"code", "message", "data"}; null when it succeeded.
  Json error_of(std::string_view method, const Json& params, bool tray) {
    const Json response = Json::parse(
        methods_->handle(ipc::make_request(1, method, params), ipc::Caller{.tray = tray}));
    return response.contains("error") ? response.at("error") : Json(nullptr);
  }

  // The plans.list entries with `kind`.
  std::vector<Json> plans(std::string_view kind = {}) {
    std::vector<Json> found;
    const Json list = call("plans.list");
    for (const Json& plan : list.at("plans")) {
      if (kind.empty() || plan.at("kind").get<std::string>() == kind) {
        found.push_back(plan);
      }
    }
    return found;
  }

  std::vector<actions::LogEntry> log(std::int64_t plan) {
    std::vector<actions::LogEntry> entries;
    EXPECT_TRUE(store_
                    ->read([&](store::Database& db) -> Result<void> {
                      entries = actions::plan_log(db, PlanId{plan}).value();
                      return {};
                    })
                    .has_value());
    return entries;
  }

  std::int64_t count(const std::string& sql) {
    std::int64_t value = -1;
    EXPECT_TRUE(store_
                    ->read([&](store::Database& db) -> Result<void> {
                      Result<store::Statement> query = db.prepare(sql);
                      if (!query) {
                        return tl::unexpected(query.error());
                      }
                      if (Result<bool> row = query->step(); !row) {
                        return tl::unexpected(row.error());
                      }
                      value = query->column_int(0);
                      return {};
                    })
                    .has_value());
    return value;
  }

  // Approves every item of `plan` as the tray does.
  Json approve_all(std::int64_t plan) {
    return call("plans.approve", Json{{"id", plan}, {"selection", {{"all", true}}}});
  }

  fakes::FakeClock& clock() { return clock_; }
  fakes::FakeFileSystem& fs() { return fs_; }
  cleanup::testing::FakeHost& handlers() { return handlers_; }
  placement::testing::FakeSystemSpace& system() { return system_; }
  placement::testing::FakeProcessRunner& tools() { return tools_; }
  placement::testing::FakeDriveFacts& drives() { return drives_; }
  startup::testing::FakeStartupSystem& startup() { return startup_; }
  startup::testing::FakeDiagnosticsLog& diagnostics() { return diagnostics_; }
  space::testing::FakeDirectoryReader& reader() { return reader_; }
  CleaningService& service() { return *service_; }
  store::Store& store() { return *store_; }
  packs::testing::FakePackFiles& pack_files() { return files_; }

  // The system volume is critically busy / the owner is logged on.
  void set_busy(bool busy) { busy_ = busy; }
  void set_owner_available(bool available) { owner_available_ = available; }
  // The owner context carries a git of its own (the service's: runs as the owner).
  clean::testing::FakeGit& owner_git() {
    owner_git_ = true;
    return git_;
  }
  // The owner context carries the owner's registry (the Uninstall entries of the programs).
  placement::testing::FakeRegistry& owner_registry() {
    owner_registry_ = true;
    return registry_;
  }

 private:
  void open_store() {
    const std::vector<store::MigrationSet> sets = {
        actions::action_migrations(), cleaning_migrations(), startup::impact_migrations()};
    store_ = store::Store::open(folder_.path() / "myclean.db", clock_, sets).value();
  }

  actions::testing::TempFolder folder_;
  fakes::FakeClock clock_{noon};
  fakes::FakeFileSystem fs_;
  FsTree tree_{fs_, noon - std::chrono::days{30}};
  clean::testing::FakeUsage usage_;
  cleanup::testing::FakeHost handlers_;
  placement::testing::FakeSystemSpace system_;
  placement::testing::FakeProcessRunner tools_;
  placement::testing::FakeDriveFacts drives_;
  startup::testing::FakeStartupSystem startup_;
  startup::testing::FakeDiagnosticsLog diagnostics_;
  space::testing::FakeDirectoryReader reader_;
  NameDigitHasher hasher_;
  packs::testing::FakePackFiles files_;
  std::unique_ptr<store::Store> store_;
  std::unique_ptr<ipc::Dispatcher> methods_;
  std::unique_ptr<CleaningService> service_;
  bool busy_ = false;
  bool owner_available_ = true;
  clean::testing::FakeGit git_;
  bool owner_git_ = false;
  placement::testing::FakeRegistry registry_;
  placement::Tools owner_tools_{.registry = &registry_};
  bool owner_registry_ = false;
};

}  // namespace myclean::cleaning::testing
