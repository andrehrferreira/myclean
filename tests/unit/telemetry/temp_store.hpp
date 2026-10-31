#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/os/clock.hpp"
#include "core/store/store.hpp"

namespace myclean::telemetry::testing {

// A store in a fresh folder under %TEMP% for the current test, removed afterwards.
class TempStore {
 public:
  explicit TempStore(const os::IClock& clock) {
    const ::testing::TestInfo* test = ::testing::UnitTest::GetInstance()->current_test_info();
    folder_ = std::filesystem::temp_directory_path() /
              ("myclean-telemetry-" + std::string(test->test_suite_name()) + "-" + test->name());
    std::filesystem::remove_all(folder_);
    std::filesystem::create_directories(folder_);
    Result<std::unique_ptr<store::Store>> opened =
        store::Store::open(folder_ / "myclean.db", clock);
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

 private:
  std::filesystem::path folder_;
  std::unique_ptr<store::Store> store_;
};

}  // namespace myclean::telemetry::testing
