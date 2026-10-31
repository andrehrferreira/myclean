#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/events/connections.hpp"
#include "core/events/event_store.hpp"
#include "core/events/events.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/process_table.hpp"

namespace myclean::events::testing {

// 2026-10-31 12:00:00 UTC.
inline const WallTime noon =
    std::chrono::sys_days{std::chrono::October / 31 / 2026} + std::chrono::hours{12};

// An event source that delivers what the test pushes, or fails to start.
class FakeEventSource final : public IEventSource {
 public:
  [[nodiscard]] Result<void> start(IEventSink& sink) override {
    if (failure_) {
      return tl::unexpected(*failure_);
    }
    sink_ = &sink;
    return {};
  }
  void stop() override {
    sink_ = nullptr;
    ++stops_;
  }

  void fail_with(Error error) { failure_ = std::move(error); }
  // Null when not started.
  [[nodiscard]] IEventSink* sink() const { return sink_; }
  [[nodiscard]] int stops() const { return stops_; }

 private:
  std::optional<Error> failure_;
  IEventSink* sink_ = nullptr;
  int stops_ = 0;
};

class FakeConnectionTable final : public IConnectionTable {
 public:
  [[nodiscard]] Result<std::vector<Connection>> snapshot() override {
    ++snapshots_;
    return result_;
  }

  void set(Result<std::vector<Connection>> result) { result_ = std::move(result); }
  [[nodiscard]] int snapshots() const { return snapshots_; }

 private:
  Result<std::vector<Connection>> result_ = std::vector<Connection>{};
  int snapshots_ = 0;
};

class FakeProcessTable final : public telemetry::IProcessTable {
 public:
  [[nodiscard]] Result<std::vector<telemetry::ProcessInfo>> snapshot() override { return result_; }
  [[nodiscard]] unsigned logical_processors() const override { return 1; }

  void set(Result<std::vector<telemetry::ProcessInfo>> result) { result_ = std::move(result); }

 private:
  Result<std::vector<telemetry::ProcessInfo>> result_ = std::vector<telemetry::ProcessInfo>{};
};

// Keeps every event it receives.
class RecordingSink final : public IEventSink {
 public:
  void on_start(const ProcessStart& event) override { starts.push_back(event); }
  void on_stop(const ProcessStop& event) override { stops.push_back(event); }
  void on_network(const NetworkTransfer& transfer) override { transfers.push_back(transfer); }

  std::vector<ProcessStart> starts;
  std::vector<ProcessStop> stops;
  std::vector<NetworkTransfer> transfers;
};

inline ProcessStart start_of(std::uint32_t pid, WallTime created, std::uint32_t parent_pid,
                             std::string image) {
  return ProcessStart{.process = {.pid = pid, .created = created},
                      .parent_pid = parent_pid,
                      .session = 1,
                      .image = std::move(image),
                      .command_line = std::nullopt};
}

inline NetworkTransfer transfer_of(std::uint32_t pid, WallTime time, Direction direction,
                                   std::uint64_t bytes) {
  return NetworkTransfer{.time = time,
                         .pid = pid,
                         .protocol = Protocol::tcp,
                         .direction = direction,
                         .remote_address = {.bytes = {192, 0, 2, 7}},
                         .remote_port = 443,
                         .bytes = bytes};
}

// Success, or a failure message carrying the error.
template <typename T>
::testing::AssertionResult ok(const Result<T>& result) {
  if (result) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
         << to_string(result.error().code) << ": " << result.error().message;
}

// A store with the events schema in a fresh folder under %TEMP%, removed afterwards.
class TempStore {
 public:
  explicit TempStore(const os::IClock& clock) {
    const ::testing::TestInfo* test = ::testing::UnitTest::GetInstance()->current_test_info();
    folder_ = std::filesystem::temp_directory_path() /
              ("myclean-events-" + std::string(test->test_suite_name()) + "-" + test->name());
    std::filesystem::remove_all(folder_);
    std::filesystem::create_directories(folder_);
    const std::vector<store::MigrationSet> sets = {event_migrations()};
    Result<std::unique_ptr<store::Store>> opened =
        store::Store::open(folder_ / "myclean.db", clock, sets);
    if (!opened) {
      ADD_FAILURE() << opened.error().message;
      return;
    }
    store_ = std::move(*opened);
  }
  TempStore(const TempStore&) = delete;
  TempStore& operator=(const TempStore&) = delete;
  TempStore(TempStore&&) = delete;
  TempStore& operator=(TempStore&&) = delete;
  ~TempStore() {
    store_.reset();
    std::error_code ignored;
    std::filesystem::remove_all(folder_, ignored);
  }

  [[nodiscard]] store::Store& get() { return *store_; }

  // Every row of `sql` as text columns (NULL as "NULL"), after flushing queued writes.
  std::vector<std::vector<std::string>> rows(std::string_view sql) {
    EXPECT_TRUE(ok(store_->flush()));
    std::vector<std::vector<std::string>> result;
    const Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
      Result<store::Statement> query = db.prepare(sql);
      if (!query) {
        return tl::unexpected(query.error());
      }
      for (;;) {
        const Result<bool> row = query->step();
        if (!row) {
          return tl::unexpected(row.error());
        }
        if (!*row) {
          return {};
        }
        std::vector<std::string>& columns = result.emplace_back();
        for (int column = 0; column < column_count(sql); ++column) {
          columns.push_back(query->is_null(column) ? "NULL" : query->column_text(column));
        }
      }
    });
    EXPECT_TRUE(ok(read));
    return result;
  }

 private:
  // The number of comma-separated columns between SELECT and FROM.
  static int column_count(std::string_view sql) {
    const std::string_view columns = sql.substr(0, sql.find(" FROM "));
    return static_cast<int>(std::ranges::count(columns, ',')) + 1;
  }

  std::filesystem::path folder_;
  std::unique_ptr<store::Store> store_;
};

}  // namespace myclean::events::testing
