#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/telemetry/memory_status.hpp"
#include "core/telemetry/perf_counters.hpp"
#include "core/telemetry/process_table.hpp"
#include "core/telemetry/volume_info.hpp"

namespace myclean::telemetry::testing {

// The process table a test sets.
class FakeProcessTable final : public IProcessTable {
 public:
  explicit FakeProcessTable(unsigned processors = 32) : processors_(processors) {}

  [[nodiscard]] Result<std::vector<ProcessInfo>> snapshot() override { return processes_; }
  [[nodiscard]] unsigned logical_processors() const override { return processors_; }

  std::vector<ProcessInfo>& processes() { return processes_; }

 private:
  std::vector<ProcessInfo> processes_;
  unsigned processors_;
};

// Counters whose values a test sets by path; every query opened shares them. values() returns
// an empty list for a path never set.
class FakeCounters final : public IPerfCounters {
 public:
  [[nodiscard]] Result<std::unique_ptr<ICounterQuery>> open() override {
    return std::make_unique<Query>(*this);
  }

  void set(std::string_view path, std::vector<CounterValue> values) {
    values_[std::string(path)] = std::move(values);
  }
  void set(std::string_view path, double value) { set(path, {CounterValue{.value = value}}); }
  // The counter does not exist on this machine: add() fails.
  void remove(std::string_view path) { missing_.emplace(path); }
  // Collects by every query so far.
  [[nodiscard]] int collects() const { return collects_; }

 private:
  class Query final : public ICounterQuery {
   public:
    explicit Query(FakeCounters& owner) : owner_(&owner) {}

    [[nodiscard]] Result<std::size_t> add(std::string_view path) override {
      if (owner_->missing_.contains(path)) {
        return fail(Errc::not_found, "no such counter");
      }
      paths_.emplace_back(path);
      return paths_.size() - 1;
    }
    [[nodiscard]] Result<void> collect() override {
      ++owner_->collects_;
      return {};
    }
    [[nodiscard]] Result<std::vector<CounterValue>> values(std::size_t index) const override {
      const auto found = owner_->values_.find(paths_.at(index));
      return found == owner_->values_.end() ? std::vector<CounterValue>{} : found->second;
    }

   private:
    FakeCounters* owner_;
    std::vector<std::string> paths_;
  };

  std::map<std::string, std::vector<CounterValue>, std::less<>> values_;
  std::set<std::string, std::less<>> missing_;
  int collects_ = 0;
};

class FakeMemoryStatus final : public IMemoryStatus {
 public:
  explicit FakeMemoryStatus(MemoryStatus status = {}) : status_(status) {}

  [[nodiscard]] Result<MemoryStatus> read() const override { return status_; }

 private:
  MemoryStatus status_;
};

class FakeVolumeInfo final : public IVolumeInfo {
 public:
  explicit FakeVolumeInfo(std::vector<VolumeStatus> volumes = {}) : volumes_(std::move(volumes)) {}

  [[nodiscard]] Result<std::vector<VolumeStatus>> volumes() const override { return volumes_; }

 private:
  std::vector<VolumeStatus> volumes_;
};

}  // namespace myclean::telemetry::testing
