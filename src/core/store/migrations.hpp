#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/store/database.hpp"

namespace myclean::store {

// One schema change. `sql` may hold several statements; it runs in one transaction.
//
// Migrations are additive only: they may create tables, indexes and triggers and add nullable or
// defaulted columns, and never drop, rename or change the type of a table or column, so that an
// older build can still open a database a newer build migrated. check_additive() enforces this
// in each module's tests.
struct Migration {
  int version = 0;
  std::string name;
  std::string sql;
};

// The migrations of one module (component), numbered 1, 2, 3, ... in order. Each component has
// its own schema version, so modules add tables without coordinating version numbers: the store
// registers "store"; the action framework, for example, registers "actions".
struct MigrationSet {
  std::string component;
  std::vector<Migration> migrations;
};

// The component's schema version in `db`: the number of the last migration applied, 0 if none.
[[nodiscard]] Result<int> schema_version(Database& db, std::string_view component);

// Applies every migration of `set` newer than the stored version, in order, each in its own
// transaction. A failed migration is rolled back, leaves the version at the last migration that
// succeeded, and is reported with its component, number and name. A database whose version is
// newer than `set` (migrated by a newer build) is left unchanged.
[[nodiscard]] Result<void> migrate(Database& db, const MigrationSet& set);

// Applies `sets` in order to an empty in-memory database and fails, naming the migration, when
// one is numbered out of order, fails to run, or drops, renames or changes the type or
// nullability of a table or column created by an earlier migration.
[[nodiscard]] Result<void> check_additive(std::span<const MigrationSet> sets);

}  // namespace myclean::store
