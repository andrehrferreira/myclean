#include "core/input/incident_store.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/input/classifier.hpp"
#include "core/input/incident.hpp"
#include "core/model/result.hpp"
#include "core/os/clock.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/schema.hpp"
#include "core/store/store.hpp"
#include "fake_probe.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::input {
namespace {

using namespace std::chrono_literals;

TEST(InputMigrations, AreAdditive) {
  const std::vector<store::MigrationSet> sets = {store::store_migrations(), input_migrations()};
  const Result<void> checked = store::check_additive(sets);
  EXPECT_TRUE(checked.has_value()) << checked.error().message;
}

// A store with the input migrations in a fresh folder under %TEMP%, removed afterwards.
class TempStore {
 public:
  explicit TempStore(const os::IClock& clock) {
    const ::testing::TestInfo* test = ::testing::UnitTest::GetInstance()->current_test_info();
    folder_ = std::filesystem::temp_directory_path() /
              ("myclean-input-" + std::string(test->test_suite_name()) + "-" + test->name());
    std::filesystem::remove_all(folder_);
    std::filesystem::create_directories(folder_);
    const std::vector<store::MigrationSet> sets = {input_migrations()};
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

 private:
  std::filesystem::path folder_;
  std::unique_ptr<store::Store> store_;
};

class IncidentStoreTest : public ::testing::Test {
 protected:
  void save(const Incident& incident) {
    const Result<std::int64_t> saved = save_incident(store(), incident);
    ASSERT_TRUE(saved.has_value()) << saved.error().message;
  }

  std::vector<FreezeGroup> summary() {
    Result<std::vector<FreezeGroup>> groups = freeze_summary(store());
    EXPECT_TRUE(groups.has_value()) << groups.error().message;
    return groups ? *std::move(groups) : std::vector<FreezeGroup>{};
  }

  // An incident `minutes_ago` minutes before noon, held open by `holder` or unexplained.
  static Incident at(int minutes_ago, std::optional<ProcessRef> holder = std::nullopt) {
    return {.chord = Chord::paste,
            .time = testing::noon - std::chrono::minutes{minutes_ago},
            .latency = 400ms,
            .holder = std::move(holder)};
  }

  store::Store& store() { return store_.get(); }

 private:
  fakes::FakeClock clock_{testing::noon};
  TempStore store_{clock_};
};

TEST_F(IncidentStoreTest, NoIncidentsIsAnEmptySummary) {
  EXPECT_TRUE(summary().empty());
}

TEST_F(IncidentStoreTest, EveryFieldRoundTrips) {
  const Incident incident{
      .chord = Chord::paste,
      .time = testing::noon,
      .latency = 5000ms,
      .capped = true,
      .owner_unanswered = 700ms,
      .input_lag = 16ms,
      .foreground = ProcessRef{.pid = 1000, .image = "Code.exe"},
      .owner = ProcessRef{.pid = 2000, .image = "Blender.exe"},
      .holder = ProcessRef{.pid = 4000, .image = "AnyDesk.exe"},
      .viewer = ProcessRef{.pid = 5000, .image = ""},
      .formats = {{.id = 13, .name = "CF_UNICODETEXT", .synthesized = false},
                  {.id = 1, .name = "CF_TEXT", .synthesized = true},
                  {.id = 0xC0A1, .name = "HTML Format \"quoted\"", .synthesized = false}},
      .hung = {{.pid = 3000, .image = "Hung.exe"}},
      .suspects = {"AnyDesk.exe", "Discord.exe"},
      .cpu_percent = 97.5,
      .memory_load_percent = 81};
  save(incident);
  const std::vector<FreezeGroup> groups = summary();
  ASSERT_EQ(groups.size(), 1U);
  ASSERT_EQ(groups[0].incidents.size(), 1U);
  EXPECT_EQ(groups[0].incidents[0].incident, incident);
  EXPECT_EQ(groups[0].incidents[0].classification, classify(incident));
  EXPECT_GT(groups[0].incidents[0].id, 0);
}

TEST_F(IncidentStoreTest, EmptyFieldsRoundTripAsNone) {
  const Incident incident{.chord = Chord::copy, .time = testing::noon, .latency = 300ms};
  save(incident);
  const std::vector<FreezeGroup> groups = summary();
  ASSERT_EQ(groups.size(), 1U);
  EXPECT_EQ(groups[0].incidents[0].incident, incident);
}

TEST_F(IncidentStoreTest, GroupsByMechanismAndProcessNewestFirst) {
  const ProcessRef remote{.pid = 4000, .image = "AnyDesk.exe"};
  for (int minutes = 10; minutes < 15; ++minutes) {
    ProcessRef holder = remote;
    holder.pid += static_cast<std::uint32_t>(minutes);  // restarts keep the image
    save(at(minutes, holder));
  }
  save(at(30));
  save(at(5));
  save(at(20, ProcessRef{.pid = 9, .image = "Other.exe"}));

  const std::vector<FreezeGroup> groups = summary();
  ASSERT_EQ(groups.size(), 3U);
  EXPECT_EQ(groups[0].mechanism, Mechanism::unexplained);
  EXPECT_EQ(groups[0].process, std::nullopt);
  EXPECT_EQ(groups[0].incidents.size(), 2U);
  EXPECT_EQ(groups[0].latest, testing::noon - 5min);
  EXPECT_EQ(groups[0].incidents[1].incident.time, testing::noon - 30min);

  EXPECT_EQ(groups[1].mechanism, Mechanism::clipboard_held_open);
  EXPECT_EQ(groups[1].process, "AnyDesk.exe");
  EXPECT_EQ(groups[1].incidents.size(), 5U);
  EXPECT_EQ(groups[1].latest, testing::noon - 10min);

  EXPECT_EQ(groups[2].process, "Other.exe");
  EXPECT_EQ(groups[2].incidents.size(), 1U);
}

TEST_F(IncidentStoreTest, MalformedRowIsReported) {
  save(at(1));
  ASSERT_TRUE(store()
                  .write([](store::Database& db) {
                    return db.exec("UPDATE input_incident SET formats = 'not json'");
                  })
                  .has_value());
  const Result<std::vector<FreezeGroup>> groups = freeze_summary(store());
  ASSERT_FALSE(groups.has_value());
  EXPECT_EQ(groups.error().code, Errc::invalid_data);
}

TEST_F(IncidentStoreTest, UnknownChordIsReported) {
  save(at(1));
  ASSERT_TRUE(store()
                  .write([](store::Database& db) {
                    return db.exec("UPDATE input_incident SET chord = 'cut'");
                  })
                  .has_value());
  const Result<std::vector<FreezeGroup>> groups = freeze_summary(store());
  ASSERT_FALSE(groups.has_value());
  EXPECT_EQ(groups.error().code, Errc::invalid_data);
}

TEST_F(IncidentStoreTest, IncidentsAreKeptForTwoYears) {
  ASSERT_TRUE(store().add_retention(input_retention()).has_value());
  Incident old = at(0);
  old.time = testing::noon - std::chrono::days{731};
  save(old);
  Incident recent = at(0);
  recent.time = testing::noon - std::chrono::days{729};
  save(recent);
  ASSERT_TRUE(store().run_retention().has_value());
  const std::vector<FreezeGroup> groups = summary();
  ASSERT_EQ(groups.size(), 1U);
  ASSERT_EQ(groups[0].incidents.size(), 1U);
  EXPECT_EQ(groups[0].incidents[0].incident.time, recent.time);
}

}  // namespace
}  // namespace myclean::input
