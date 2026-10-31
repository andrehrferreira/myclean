# The action framework: plan state machine, append-only audit log, undo data, quarantine and the
# file actions DeleteItems and QuarantineItems (docs/architecture.md section 6).
myclean_add_library(actions
  SOURCES
    audit_log.cpp
    audit_log.hpp
    delete_items.cpp
    delete_items.hpp
    item_action.cpp
    item_action.hpp
    policy.cpp
    policy.hpp
    quarantine.cpp
    quarantine.hpp
    schema.cpp
    schema.hpp
    store_plan_source.cpp
    store_plan_source.hpp
  DEPENDS
    myclean::model
    myclean::os
    myclean::safety
    myclean::store)
