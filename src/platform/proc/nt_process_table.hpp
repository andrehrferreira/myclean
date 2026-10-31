#pragma once

#include <cstddef>
#include <vector>

#include "core/model/result.hpp"
#include "core/telemetry/process_table.hpp"

namespace myclean::platform {

// The process table from one NtQuerySystemInformation(SystemProcessInformation) call per
// snapshot. The function is resolved from ntdll at run time: a static import of Nt* functions
// trips antivirus heuristics (docs/spikes/X4-jobs.md). Works without elevation; not thread-safe.
class NtProcessTable final : public telemetry::IProcessTable {
 public:
  NtProcessTable();

  [[nodiscard]] Result<std::vector<telemetry::ProcessInfo>> snapshot() override;
  [[nodiscard]] unsigned logical_processors() const override;

 private:
  using QueryFunction = long(__stdcall*)(int, void*, unsigned long, unsigned long*);

  QueryFunction query_ = nullptr;
  std::vector<std::byte> buffer_;  // reused, grown when the table outgrows it
};

}  // namespace myclean::platform
