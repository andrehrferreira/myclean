#include "core/store/database.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/store/store.hpp"
#include "fakes/fake_clock.hpp"
#include "store_test_support.hpp"

namespace myclean::store {
namespace {

using testing::count_rows;
using testing::noon;
using testing::ok;
using testing::TempDir;

std::vector<char> read_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

TEST(Database, DefaultLocationIsUnderProgramData) {
  EXPECT_EQ(database_path("C:\\ProgramData"),
            std::filesystem::path("C:\\ProgramData\\MyClean\\myclean.db"));
}

TEST(Database, FirstOpenCreatesTheFileInWalMode) {
  const TempDir dir;
  const fakes::FakeClock clock(noon);
  ASSERT_FALSE(std::filesystem::exists(dir.database()));

  Result<std::unique_ptr<Store>> store = Store::open(dir.database(), clock);
  ASSERT_TRUE(ok(store));

  EXPECT_TRUE(std::filesystem::exists(dir.database()));
  std::string mode;
  ASSERT_TRUE(ok((*store)->read([&](Database& db) -> Result<void> {
    Result<Statement> query = db.prepare("PRAGMA journal_mode");
    if (!query) {
      return tl::unexpected(query.error());
    }
    if (Result<bool> row = query->step(); !row) {
      return tl::unexpected(row.error());
    }
    mode = query->column_text(0);
    return {};
  })));
  EXPECT_EQ(mode, "wal");
}

TEST(Database, HealthyDatabaseOpensForReadsAndWrites) {
  const TempDir dir;
  const fakes::FakeClock clock(noon);
  {
    Result<std::unique_ptr<Store>> store = Store::open(dir.database(), clock);
    ASSERT_TRUE(ok(store));
    ASSERT_TRUE(ok(
        testing::exec(**store, "CREATE TABLE note(text TEXT); INSERT INTO note VALUES('kept')")));
  }
  Result<std::unique_ptr<Store>> reopened = Store::open(dir.database(), clock);
  ASSERT_TRUE(ok(reopened));
  EXPECT_EQ(count_rows(**reopened, "note"), 1);
  EXPECT_TRUE(ok(testing::exec(**reopened, "INSERT INTO note VALUES('more')")));
  EXPECT_EQ(count_rows(**reopened, "note"), 2);
}

TEST(Database, CorruptDatabaseIsRefusedAndLeftUntouched) {
  const TempDir dir;
  {
    Result<Database> db = Database::open(dir.database());
    ASSERT_TRUE(ok(db));
    ASSERT_TRUE(ok(
        db->exec("CREATE TABLE item(id INTEGER PRIMARY KEY, text TEXT NOT NULL);"
                 "CREATE INDEX item_text ON item(text);"
                 "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 2000) "
                 "INSERT INTO item(text) SELECT hex(randomblob(40)) FROM n;")));
  }  // closing the last connection checkpoints the WAL into the file
  constexpr std::size_t page_size = 4096;
  std::vector<char> bytes = read_file(dir.database());
  ASSERT_GT(bytes.size(), 20 * page_size);
  for (std::size_t offset = 10 * page_size; offset < 12 * page_size; ++offset) {
    bytes[offset] = static_cast<char>(0x5A);
  }
  {
    std::ofstream file(dir.database(), std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  const fakes::FakeClock clock(noon);
  const Result<std::unique_ptr<Store>> store = Store::open(dir.database(), clock);

  ASSERT_FALSE(store.has_value());
  EXPECT_EQ(store.error().code, Errc::invalid_data);
  EXPECT_NE(store.error().message.find(dir.database().string()), std::string::npos)
      << store.error().message;
  EXPECT_NE(store.error().message.find("integrity_check"), std::string::npos)
      << store.error().message;
  EXPECT_EQ(read_file(dir.database()), bytes);
}

TEST(Database, TransactionCommitsOnSuccessAndRollsBackOnFailure) {
  Result<Database> db = Database::open_in_memory();
  ASSERT_TRUE(ok(db));
  ASSERT_TRUE(ok(db->exec("CREATE TABLE note(text TEXT)")));

  ASSERT_TRUE(ok(db->transaction([&] { return db->exec("INSERT INTO note VALUES('a')"); })));
  const Result<void> failed = db->transaction([&]() -> Result<void> {
    if (Result<void> inserted = db->exec("INSERT INTO note VALUES('b')"); !inserted) {
      return inserted;
    }
    return fail(Errc::invalid_state, "abandoned");
  });
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().message, "abandoned");

  // Nested: the inner failure undoes only the inner part.
  ASSERT_TRUE(ok(db->transaction([&]() -> Result<void> {
    if (Result<void> inserted = db->exec("INSERT INTO note VALUES('c')"); !inserted) {
      return inserted;
    }
    static_cast<void>(db->transaction([&]() -> Result<void> {
      static_cast<void>(db->exec("INSERT INTO note VALUES('d')"));
      return fail(Errc::invalid_state, "inner");
    }));
    return {};
  })));

  Result<Statement> query = db->prepare("SELECT group_concat(text, '') FROM note");
  ASSERT_TRUE(ok(query));
  ASSERT_TRUE(ok(query->step()));
  EXPECT_EQ(query->column_text(0), "ac");
}

TEST(Database, EmptyTextIsStoredAsEmptyNotNull) {
  Result<Database> db = Database::open_in_memory();
  ASSERT_TRUE(ok(db));
  Result<Statement> query = db->prepare("SELECT ?1 IS NULL, length(?1)");
  ASSERT_TRUE(ok(query));
  query->bind(1, std::string_view{});
  ASSERT_TRUE(ok(query->step()));
  EXPECT_EQ(query->column_int(0), 0);
  EXPECT_EQ(query->column_int(1), 0);
}

TEST(Database, UnixSecondsRoundTrip) {
  EXPECT_EQ(to_unix_seconds(noon), 1'793'448'000);
  EXPECT_EQ(from_unix_seconds(1'793'448'000), noon);
}

}  // namespace
}  // namespace myclean::store
