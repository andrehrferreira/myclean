#include "core/store/migrations.hpp"

#include <cstddef>
#include <format>
#include <map>
#include <span>
#include <string>
#include <string_view>

#include "core/model/result.hpp"
#include "core/store/database.hpp"

namespace myclean::store {
namespace {

std::string label(const MigrationSet& set, const Migration& migration) {
  return set.component + " " + std::to_string(migration.version) + " (" + migration.name + ")";
}

Result<void> validate(const MigrationSet& set) {
  if (set.component.empty()) {
    return fail(Errc::invalid_argument, "a migration set needs a component name");
  }
  for (std::size_t index = 0; index < set.migrations.size(); ++index) {
    const Migration& migration = set.migrations[index];
    if (migration.version != static_cast<int>(index) + 1) {
      return fail(Errc::invalid_argument, "migration " + label(set, migration) +
                                              " is out of order: expected number " +
                                              std::to_string(index + 1));
    }
  }
  return {};
}

struct Column {
  std::string type;
  bool not_null = false;
};

// Table name -> column name -> declared shape.
using Schema = std::map<std::string, std::map<std::string, Column>>;

Result<Schema> read_schema(Database& db) {
  Result<Statement> tables =
      db.prepare("SELECT name FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite%'");
  if (!tables) {
    return tl::unexpected(tables.error());
  }
  Schema schema;
  Result<bool> table = tables->step();
  for (; table && *table; table = tables->step()) {
    const std::string name = tables->column_text(0);
    Result<Statement> columns =
        db.prepare("SELECT name, type, \"notnull\" FROM pragma_table_info(?1)");
    if (!columns) {
      return tl::unexpected(columns.error());
    }
    columns->bind(1, name);
    auto& shape = schema[name];
    Result<bool> column = columns->step();
    for (; column && *column; column = columns->step()) {
      shape[columns->column_text(0)] =
          Column{.type = columns->column_text(1), .not_null = columns->column_int(2) != 0};
    }
    if (!column) {
      return tl::unexpected(column.error());
    }
  }
  if (!table) {
    return tl::unexpected(table.error());
  }
  return schema;
}

// Describes the first table or column of `before` that `after` lost or changed; empty if none.
std::string first_destructive_change(const Schema& before, const Schema& after) {
  for (const auto& [table, columns] : before) {
    const auto kept = after.find(table);
    if (kept == after.end()) {
      return "table " + table + " was dropped or renamed";
    }
    for (const auto& [name, column] : columns) {
      const auto found = kept->second.find(name);
      const std::string where = std::format("column {}.{}", table, name);
      if (found == kept->second.end()) {
        return where + " was dropped or renamed";
      }
      if (found->second.type != column.type) {
        return std::format("{} changed type from '{}' to '{}'", where, column.type,
                           found->second.type);
      }
      if (found->second.not_null != column.not_null) {
        return where + " changed nullability";
      }
    }
  }
  return {};
}

}  // namespace

Result<int> schema_version(Database& db, std::string_view component) {
  Result<Statement> exists = db.prepare(
      "SELECT count(*) FROM sqlite_schema WHERE type = 'table' AND name = 'schema_version'");
  if (!exists) {
    return tl::unexpected(exists.error());
  }
  if (Result<bool> row = exists->step(); !row) {
    return tl::unexpected(row.error());
  }
  if (exists->column_int(0) == 0) {
    return 0;
  }
  Result<Statement> query = db.prepare("SELECT version FROM schema_version WHERE component = ?1");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, component);
  const Result<bool> row = query->step();
  if (!row) {
    return tl::unexpected(row.error());
  }
  return *row ? static_cast<int>(query->column_int(0)) : 0;
}

Result<void> migrate(Database& db, const MigrationSet& set) {
  if (Result<void> valid = validate(set); !valid) {
    return valid;
  }
  if (Result<void> created = db.exec("CREATE TABLE IF NOT EXISTS schema_version("
                                     "component TEXT PRIMARY KEY, version INTEGER NOT NULL)");
      !created) {
    return created;
  }
  const Result<int> current = schema_version(db, set.component);
  if (!current) {
    return tl::unexpected(current.error());
  }
  for (const Migration& migration : set.migrations) {
    if (migration.version <= *current) {
      continue;
    }
    const Result<void> applied = db.transaction([&]() -> Result<void> {
      if (Result<void> ran = db.exec(migration.sql); !ran) {
        return ran;
      }
      Result<Statement> record = db.prepare(
          "INSERT INTO schema_version(component, version) VALUES(?1, ?2) "
          "ON CONFLICT(component) DO UPDATE SET version = excluded.version");
      if (!record) {
        return tl::unexpected(record.error());
      }
      return record->bind(1, set.component).bind(2, migration.version).run();
    });
    if (!applied) {
      return fail(applied.error().code,
                  "migration " + label(set, migration) + " failed: " + applied.error().message);
    }
  }
  return {};
}

Result<void> check_additive(std::span<const MigrationSet> sets) {
  Result<Database> db = Database::open_in_memory();
  if (!db) {
    return tl::unexpected(db.error());
  }
  for (const MigrationSet& set : sets) {
    if (Result<void> valid = validate(set); !valid) {
      return valid;
    }
    for (const Migration& migration : set.migrations) {
      const Result<Schema> before = read_schema(*db);
      if (!before) {
        return tl::unexpected(before.error());
      }
      if (Result<void> ran = db->exec(migration.sql); !ran) {
        return fail(Errc::invalid_argument,
                    "migration " + label(set, migration) +
                        " fails on an empty database: " + ran.error().message);
      }
      const Result<Schema> after = read_schema(*db);
      if (!after) {
        return tl::unexpected(after.error());
      }
      if (const std::string problem = first_destructive_change(*before, *after); !problem.empty()) {
        return fail(Errc::invalid_argument,
                    "migration " + label(set, migration) + " is not additive: " + problem);
      }
    }
  }
  return {};
}

}  // namespace myclean::store
