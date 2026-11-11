#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/events/event_store.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/security/network.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

namespace myclean::security::testing {

inline const WallTime noon =
    std::chrono::sys_days{std::chrono::November / 10 / 2026} + std::chrono::hours{12};

inline bool ok(const Result<void>& result) {
  if (!result) {
    ADD_FAILURE() << result.error().message;
  }
  return result.has_value();
}

template <typename T>
bool ok(const Result<T>& result) {
  if (!result) {
    ADD_FAILURE() << result.error().message;
  }
  return result.has_value();
}

// A store in a temporary folder with the events and security tables.
class TempStore {
 public:
  explicit TempStore(const os::IClock& clock) {
    const ::testing::TestInfo* test = ::testing::UnitTest::GetInstance()->current_test_info();
    folder_ = std::filesystem::temp_directory_path() /
              ("myclean-security-" + std::string(test->test_suite_name()) + "-" + test->name());
    std::filesystem::remove_all(folder_);
    std::filesystem::create_directories(folder_);
    const std::vector<store::MigrationSet> sets = {events::event_migrations(),
                                                   security_migrations()};
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

  // The number of rows `sql` returns, after flushing queued writes.
  std::size_t count(std::string_view sql) {
    EXPECT_TRUE(ok(store_->flush()));
    std::size_t rows = 0;
    EXPECT_TRUE(ok(store_->read([&](store::Database& db) -> Result<void> {
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
        ++rows;
      }
    })));
    return rows;
  }

 private:
  std::filesystem::path folder_;
  std::unique_ptr<store::Store> store_;
};

}  // namespace myclean::security::testing
