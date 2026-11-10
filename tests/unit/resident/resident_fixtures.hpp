#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/units.hpp"
#include "core/resident/history.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/process_collector.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::resident::testing {

using namespace std::chrono_literals;

inline const WallTime noon = std::chrono::sys_days{std::chrono::November / 10 / 2026} + 12h;

// One process of a fake process table.
struct Proc {
  std::uint32_t pid = 0;
  std::string image;
  std::chrono::seconds cpu{};
  std::uint64_t io_bytes = 0;
  Bytes private_bytes;
  Bytes ram;
  WallTime created = noon - 24h;
};

// A process table at `time` whose trees are `trees`, each root first.
inline telemetry::ProcessView view_of(WallTime time, const std::vector<std::vector<Proc>>& trees) {
  telemetry::ProcessView view{.time = time};
  for (const std::vector<Proc>& tree : trees) {
    telemetry::ProcessTree entry;
    for (const Proc& proc : tree) {
      entry.members.push_back(view.rows.size());
      view.rows.push_back(telemetry::ProcessRow{.info = {.pid = proc.pid,
                                                         .created = proc.created,
                                                         .image = proc.image,
                                                         .cpu_time = proc.cpu,
                                                         .private_bytes = proc.private_bytes,
                                                         .private_working_set = proc.ram,
                                                         .io_bytes = proc.io_bytes}});
    }
    view.trees.push_back(std::move(entry));
  }
  return view;
}

// A store with the resident tables in a folder of the test's own.
class StoreTest : public ::testing::Test {
 public:
  StoreTest()
      : folder_(std::filesystem::temp_directory_path() /
                ("myclean-resident-" +
                 std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()))) {
    std::filesystem::remove_all(folder_);
    std::filesystem::create_directories(folder_);
    open();
  }
  StoreTest(const StoreTest&) = delete;
  StoreTest& operator=(const StoreTest&) = delete;
  StoreTest(StoreTest&&) = delete;
  StoreTest& operator=(StoreTest&&) = delete;
  ~StoreTest() override {
    store_.reset();
    std::error_code ignored;
    std::filesystem::remove_all(folder_, ignored);
  }

 protected:
  store::Store& store() { return *store_; }
  fakes::FakeClock& clock() { return clock_; }
  // Closes the store and opens it again, as a service restart does.
  void reopen() {
    ASSERT_TRUE(store_->close().has_value());
    store_.reset();
    open();
  }

 private:
  void open() {
    const std::array sets{resident_migrations()};
    store_ = store::Store::open(folder_ / "myclean.db", clock_, sets).value();
  }

  std::filesystem::path folder_;
  fakes::FakeClock clock_{noon};
  std::unique_ptr<store::Store> store_;
};

}  // namespace myclean::resident::testing
