#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <ratio>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/cleanup/command_runner.hpp"
#include "core/cleanup/handler_cleanup.hpp"
#include "core/cleanup/handler_host.hpp"
#include "core/cleanup/recycle_bin.hpp"
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

// FILETIME ticks of `time`.
inline std::uint64_t filetime_of(WallTime time) {
  constexpr std::uint64_t epoch_ticks = 116444736000000000ULL;
  const auto ticks =
      std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>>(
          time.time_since_epoch());
  return epoch_ticks + static_cast<std::uint64_t>(ticks.count());
}

// A $I record as Explorer writes it: version 2 (int32 length, the path and its NUL) or 1 (the
// path NUL-padded to 260 code units).
inline std::vector<std::uint8_t> bin_record(int version, std::uint64_t size, WallTime deleted,
                                            std::wstring_view original) {
  std::vector<std::uint8_t> data;
  const auto put = [&data](std::uint64_t value, int width) {
    for (int index = 0; index < width; ++index) {
      data.push_back(static_cast<std::uint8_t>(value >> (8U * static_cast<unsigned>(index))));
    }
  };
  put(static_cast<std::uint64_t>(version), 8);
  put(size, 8);
  put(filetime_of(deleted), 8);
  const std::size_t units = version == 1 ? 260 : original.size() + 1;
  if (version != 1) {
    put(units, 4);
  }
  for (std::size_t index = 0; index < units; ++index) {
    put(index < original.size() ? static_cast<std::uint64_t>(original[index]) : 0U, 2);
  }
  return data;
}

// The owner's $I files, fixed.
class FakeBinRecords final : public IBinRecords {
 public:
  [[nodiscard]] Result<std::vector<BinFile>> read(const std::stop_token& /*stop*/) const override {
    ++reads;
    if (failure) {
      return tl::unexpected(*failure);
    }
    return files;
  }

  std::vector<BinFile> files;
  std::optional<Error> failure;
  mutable int reads = 0;
};

// A Recycle Bin of fixed items; remove() takes them out unless they fail.
class FakeRecycleBin final : public IRecycleBin {
 public:
  [[nodiscard]] Result<std::vector<BinItem>> items(const std::stop_token& /*stop*/) override {
    if (failure) {
      return tl::unexpected(*failure);
    }
    return contents;
  }

  [[nodiscard]] Result<std::vector<Skipped>> remove(std::span<const std::string> ids,
                                                    const std::stop_token& /*stop*/) override {
    if (failure) {
      return tl::unexpected(*failure);
    }
    std::vector<Skipped> left;
    for (const std::string& id : ids) {
      removed.push_back(id);
      const auto item = std::ranges::find(contents, id, &BinItem::id);
      if (const auto error = failures.find(id); error != failures.end()) {
        left.push_back(
            {.target = item == contents.end() ? id : item->name, .error = error->second});
      } else if (item == contents.end()) {
        left.push_back(
            {.target = id,
             .error = {.code = Errc::not_found, .message = "no longer in the Recycle Bin"}});
      } else {
        contents.erase(item);
      }
    }
    return left;
  }

  std::vector<BinItem> contents;
  std::map<std::string, Error> failures;
  std::optional<Error> failure;  // the bin cannot be read
  std::vector<std::string> removed;
};

// Records the files sent to the Recycle Bin and hands each to `on_recycle` (a test takes it out
// of its fake file system), unless it is in `failures`.
class FakeRecycler final : public IRecycler {
 public:
  [[nodiscard]] Result<std::vector<Skipped>> recycle(std::span<const std::filesystem::path> files,
                                                     const std::stop_token& /*stop*/) override {
    std::vector<Skipped> left;
    for (const std::filesystem::path& file : files) {
      sent.push_back(file);
      const std::u8string text = file.u8string();
      if (const auto error = failures.find(file); error != failures.end()) {
        left.push_back({.target = std::string(text.begin(), text.end()), .error = error->second});
      } else if (on_recycle) {
        on_recycle(file);
      }
    }
    return left;
  }

  std::function<void(const std::filesystem::path&)> on_recycle;
  std::map<std::filesystem::path, Error> failures;
  std::vector<std::filesystem::path> sent;
};

}  // namespace myclean::cleanup::testing
