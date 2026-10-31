# SQLite store: connection, migrations, batched and synchronous writes, time series, retention,
# and the repositories of findings, plans and scheduler state.
myclean_add_library(store
  SOURCES
    database.cpp
    database.hpp
    migrations.cpp
    migrations.hpp
    plan_repository.cpp
    plan_repository.hpp
    schedule_repository.cpp
    schedule_repository.hpp
    schema.cpp
    schema.hpp
    store.cpp
    store.hpp
    time_series.cpp
    time_series.hpp
  DEPENDS
    myclean::model
    myclean::os
    unofficial::sqlite3::sqlite3)
