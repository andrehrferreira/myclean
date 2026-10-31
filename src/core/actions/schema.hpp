#pragma once

#include "core/store/migrations.hpp"

namespace myclean::actions {

// The action framework's tables (component "actions"), registered with store::Store::open:
//   action_log  append-only audit of every plan transition and item outcome; triggers abort
//               any UPDATE or DELETE. No foreign key to `plan`: retention deletes old plans,
//               the audit stays.
//   undo_blob   the undo data of an applied plan, linked to its action_log entry
//   quarantine  every item moved to quarantine: original path, location, identity, expiry
[[nodiscard]] const store::MigrationSet& action_migrations();

}  // namespace myclean::actions
