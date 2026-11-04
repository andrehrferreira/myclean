#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::telemetry {

// One process as the OS reports it at one instant. Counters are totals since the process
// started; the collector turns them into rates.
struct ProcessInfo {
  std::uint32_t pid = 0;
  std::uint32_t parent_pid = 0;  // the creator's PID, which may since have exited or been reused
  WallTime created;
  std::string image;                    // file name only ("chrome.exe"), UTF-8
  std::chrono::nanoseconds cpu_time{};  // user + kernel
  Bytes private_bytes;                  // commit charge
  Bytes working_set;
  Bytes private_working_set;
  std::uint64_t io_bytes = 0;  // read + write + other transfers
  std::uint32_t handles = 0;
  std::uint32_t threads = 0;
  std::uint32_t ready_threads = 0;  // threads ready to run, waiting for a processor
  // The Terminal Services session; 0 is the services' session. nullopt: not reported.
  std::optional<std::uint32_t> session;
};

// The process table. Implemented by src/platform/proc.
class IProcessTable {
 public:
  virtual ~IProcessTable() = default;
  IProcessTable(const IProcessTable&) = delete;
  IProcessTable& operator=(const IProcessTable&) = delete;
  IProcessTable(IProcessTable&&) = delete;
  IProcessTable& operator=(IProcessTable&&) = delete;

  // Every process, read in one consistent snapshot.
  [[nodiscard]] virtual Result<std::vector<ProcessInfo>> snapshot() = 0;
  // Logical processors in all processor groups; CPU percentages are relative to all of them.
  [[nodiscard]] virtual unsigned logical_processors() const = 0;

 protected:
  IProcessTable() = default;
};

}  // namespace myclean::telemetry
