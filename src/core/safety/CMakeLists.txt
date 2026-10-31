# SafeRemove: the only code path that deletes or moves files (docs/architecture.md section 6).
myclean_add_library(safety
  SOURCES
    deny_list.cpp
    deny_list.hpp
    paths.cpp
    paths.hpp
    plan_source.hpp
    safe_remove.cpp
    safe_remove.hpp
  DEPENDS
    myclean::os)
