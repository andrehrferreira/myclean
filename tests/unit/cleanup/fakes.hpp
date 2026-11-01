#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/cleanup/command_runner.hpp"
#include "core/cleanup/handler_host.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::cleanup::testing {

// Answers commands from a table keyed by "<program> <arguments>"; records every call.
class FakeRunner final : public ICommandRunner {
 public:
  [[nodiscard]] bool user_context() const override { return user; }

  [[nodiscard]] Result<CommandOutput> run(const Command& command,
                                          std::stop_token /*stop*/) override {
    std::string key = command.program;
    for (const std::string& argument : command.arguments) {
      key += " " + argument;
    }
    calls.push_back(key);
    arguments.push_back(command.arguments);
    timeouts.push_back(command.timeout);
    if (on_run) {
      on_run(key);
    }
    const auto found = answers.find(key);
    if (found == answers.end()) {
      return fail(Errc::not_found, "no answer for " + key);
    }
    return found->second;
  }

  // An answer that exits with `code` and prints `out` and `err`.
  void answer(const std::string& key, std::string_view out, std::uint32_t code = 0,
              std::string_view err = {}) {
    answers.insert_or_assign(
        key, CommandOutput{.exit_code = code, .out = std::string(out), .err = std::string(err)});
  }

  bool user = true;
  std::map<std::string, Result<CommandOutput>> answers;
  std::function<void(const std::string&)> on_run;
  std::vector<std::string> calls;
  std::vector<std::vector<std::string>> arguments;  // of each call, as passed
  std::vector<std::chrono::milliseconds> timeouts;
};

// Handlers with fixed sizes; a successful purge sets the size to zero.
class FakeHost final : public IHandlerHost {
 public:
  [[nodiscard]] Result<Bytes> space_used(std::string_view handler,
                                         const std::filesystem::path& volume,
                                         std::stop_token /*stop*/) override {
    volumes.push_back(volume);
    const auto found = sizes.find(std::string(handler));
    if (found == sizes.end()) {
      return fail(Errc::not_found, "no handler " + std::string(handler));
    }
    return found->second;
  }

  [[nodiscard]] Result<void> purge(std::string_view handler,
                                   const std::filesystem::path& /*volume*/,
                                   std::stop_token /*stop*/) override {
    const std::string name(handler);
    purged.push_back(name);
    if (const auto failure = failures.find(name); failure != failures.end()) {
      return tl::unexpected(failure->second);
    }
    sizes.insert_or_assign(name, Bytes{});
    return {};
  }

  std::map<std::string, Result<Bytes>> sizes;
  std::map<std::string, Error> failures;
  std::vector<std::string> purged;
  std::vector<std::filesystem::path> volumes;
};

}  // namespace myclean::cleanup::testing
