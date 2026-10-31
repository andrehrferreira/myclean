#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "core/cleanup/command_runner.hpp"
#include "core/compress/compression.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"

namespace myclean::compress::testing {

inline FileIdentity identity_of(std::uint8_t id) {
  FileIdentity identity{.volume_serial = 7};
  identity.file_id.at(0) = id;
  return identity;
}

// Compresses every file at a fixed ratio per path (default `ratio`); records each read.
class FakeProbe final : public ICompressionProbe {
 public:
  struct Read {
    std::filesystem::path file;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
  };

  [[nodiscard]] Result<SampleSizes> sample(const std::filesystem::path& file, std::uint64_t offset,
                                           std::uint64_t length) const override {
    reads.push_back({.file = file, .offset = offset, .length = length});
    if (on_sample) {
      on_sample();
    }
    if (failing.contains(file)) {
      return fail(Errc::in_use, "locked: " + file.string());
    }
    const auto found = ratios.find(file);
    const double base = found == ratios.end() ? ratio : found->second;
    const auto scaled = [&](double factor) {
      return Bytes{
          static_cast<std::uint64_t>(static_cast<double>(length) * std::min(1.0, base * factor))};
    };
    return SampleSizes{.read = Bytes{length},
                       .xpress4k = scaled(1.0),
                       .xpress8k = scaled(0.95),
                       .xpress16k = scaled(0.9),
                       .xpress32k = scaled(0.85),
                       .lznt1 = scaled(1.1)};
  }

  double ratio = 0.5;
  std::map<std::filesystem::path, double> ratios;
  std::set<std::filesystem::path> failing;
  std::function<void()> on_sample;
  mutable std::vector<Read> reads;
};

// Files with a state each; compress halves the size on disk, decompress restores it.
class FakeWof final : public IWof {
 public:
  void add(const std::filesystem::path& file, FileIdentity identity, Bytes size) {
    files.insert_or_assign(file,
                           FileState{.identity = identity, .size = size, .size_on_disk = size});
  }

  [[nodiscard]] Result<FileState> state(const std::filesystem::path& file) const override {
    const auto found = files.find(file);
    if (found == files.end()) {
      return fail(Errc::not_found, "no file " + file.string());
    }
    return found->second;
  }

  [[nodiscard]] Result<bool> compress(const std::filesystem::path& file, Algorithm algorithm,
                                      const FileIdentity& expected) override {
    compressed.push_back(file);
    if (in_use.contains(file)) {
      return fail(Errc::in_use, "open for writing: " + file.string());
    }
    const auto found = files.find(file);
    if (found == files.end() || found->second.identity != expected) {
      return fail(Errc::refused, "another file now: " + file.string());
    }
    if (not_beneficial.contains(file)) {
      return false;
    }
    found->second.wof = algorithm;
    found->second.external = true;
    found->second.size_on_disk = Bytes{found->second.size.count() / 2};
    return true;
  }

  [[nodiscard]] Result<void> decompress(const std::filesystem::path& file) override {
    decompressed.push_back(file);
    if (const auto failure = failures.find(file); failure != failures.end()) {
      return tl::unexpected(failure->second);
    }
    FileState& state = files.at(file);
    state.wof.reset();
    state.external = false;
    state.size_on_disk = state.size;
    return {};
  }

  [[nodiscard]] Result<bool> compact_os() const override { return compact_os_on; }

  std::map<std::filesystem::path, FileState> files;
  std::set<std::filesystem::path> in_use;
  std::set<std::filesystem::path> not_beneficial;
  std::map<std::filesystem::path, Error> failures;
  std::vector<std::filesystem::path> compressed;
  std::vector<std::filesystem::path> decompressed;
  bool compact_os_on = false;
};

// Directory listings from a table keyed by folder path.
class FakeReader final : public space::IDirectoryReader {
 public:
  [[nodiscard]] Result<std::vector<space::ListedEntry>> list(
      const std::filesystem::path& path) const override {
    const auto found = folders.find(path);
    if (found == folders.end()) {
      return fail(Errc::not_found, "no folder " + path.string());
    }
    return found->second;
  }

  std::map<std::filesystem::path, std::vector<space::ListedEntry>> folders;
};

// Records the commands it runs; `on_run` may change the world before the runner returns.
class FakeRunner final : public cleanup::ICommandRunner {
 public:
  [[nodiscard]] bool user_context() const override { return false; }

  [[nodiscard]] Result<cleanup::CommandOutput> run(const cleanup::Command& command,
                                                   std::stop_token /*stop*/) override {
    std::string line = command.program;
    for (const std::string& argument : command.arguments) {
      line += " " + argument;
    }
    calls.push_back(line);
    if (on_run) {
      on_run(line);
    }
    return output;
  }

  cleanup::CommandOutput output;
  std::function<void(const std::string&)> on_run;
  std::vector<std::string> calls;
};

}  // namespace myclean::compress::testing
