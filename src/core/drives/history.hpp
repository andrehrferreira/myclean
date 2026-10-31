#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

// The storage optimizer's own record of past runs: the Application event log of the
// Microsoft-Windows-Defrag provider, readable without administrator rights.
namespace myclean::drives {

enum class OptimizationKind { retrim, defrag, other };

[[nodiscard]] std::string_view to_string(OptimizationKind kind);

// Event 258 (an optimization finished) and 264 (it failed) as logged.
inline constexpr std::uint32_t event_optimized = 258;
inline constexpr std::uint32_t event_failed = 264;

// One optimizer event as the log holds it.
struct OptimizerEvent {
  WallTime time;
  std::uint32_t id = 0;
  std::string operation;           // first insert, in the language Windows logged it in
  std::string volume;              // second insert: "Backup 1 (K:)"
  std::vector<std::uint8_t> data;  // the binary data
};

struct Optimization {
  WallTime time;
  char drive = 0;  // upper-case drive letter
  OptimizationKind kind = OptimizationKind::other;
  bool succeeded = false;
  std::string operation;  // as logged
};

// The optimization `event` records; nullopt for another event id or a volume without a drive
// letter. The binary data holds the result (HRESULT, 0 = success) and then the operation code,
// both little-endian 32-bit: 0x2BC is a retrim and 0x1ED a defragmentation (observed on
// Windows 11 26200, any display language). An unknown code falls back to the English operation
// name ("retrim", "defragmentation") and else to `other`.
[[nodiscard]] std::optional<Optimization> interpret(const OptimizerEvent& event);

// Reads the optimizer events of the Application log. Implemented by src/platform/defrag.
class IOptimizerLog {
 public:
  virtual ~IOptimizerLog() = default;
  IOptimizerLog(const IOptimizerLog&) = delete;
  IOptimizerLog& operator=(const IOptimizerLog&) = delete;
  IOptimizerLog(IOptimizerLog&&) = delete;
  IOptimizerLog& operator=(IOptimizerLog&&) = delete;

  // Events 258 and 264 still in the log, in any order.
  [[nodiscard]] virtual Result<std::vector<OptimizerEvent>> events() const = 0;

 protected:
  IOptimizerLog() = default;
};

// The newest successful runs on one drive, and how far back the log reaches.
struct LastRuns {
  std::optional<WallTime> retrim;
  std::optional<WallTime> defrag;
  std::optional<WallTime> log_since;  // the oldest optimization in the log, of any drive
};

[[nodiscard]] LastRuns last_runs(std::span<const Optimization> history, char drive);

}  // namespace myclean::drives
