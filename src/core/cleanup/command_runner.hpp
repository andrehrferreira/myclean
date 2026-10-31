#pragma once

#include <chrono>
#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

#include "core/model/result.hpp"

namespace myclean::cleanup {

// One external command. `program` is a bare tool name ("pnpm"), resolved on PATH by the runner,
// or an absolute path.
struct Command {
  std::string program;
  std::vector<std::string> arguments;
  std::chrono::milliseconds timeout{std::chrono::minutes(5)};
};

struct CommandOutput {
  std::uint32_t exit_code = 0;  // DISM returns HRESULTs, hence unsigned
  std::string out;              // standard output, UTF-8
  std::string err;              // standard error, UTF-8
};

// Runs external commands without a window and without input. Implemented by
// platform/cleanup (Win32CommandRunner) and by test fakes.
class ICommandRunner {
 public:
  virtual ~ICommandRunner() = default;
  ICommandRunner(const ICommandRunner&) = delete;
  ICommandRunner& operator=(const ICommandRunner&) = delete;
  ICommandRunner(ICommandRunner&&) = delete;
  ICommandRunner& operator=(ICommandRunner&&) = delete;

  // True when commands run under a user's own token: false for LocalSystem and the other
  // service accounts.
  [[nodiscard]] virtual bool user_context() const = 0;

  // Runs `command` to its end and returns its exit code and output. Fails with not_found when
  // the program cannot be resolved, io_error when it cannot start or its timeout passes, and
  // refused when `stop` is requested; in the last two cases the whole process tree is ended.
  [[nodiscard]] virtual Result<CommandOutput> run(const Command& command, std::stop_token stop) = 0;

 protected:
  ICommandRunner() = default;
};

}  // namespace myclean::cleanup
