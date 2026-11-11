#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/events/events.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/packs/loader.hpp"
#include "core/rules/schema.hpp"
#include "core/trust/trust.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

// The OS facts the trust component needs; Win32 in the service, fakes in tests.
struct TrustSources {
  std::unique_ptr<trust::IImageInspector> inspector;
  std::unique_ptr<trust::IProcessImages> images;
  // The folders a standard user can write to, or why they are unknown (then every path is
  // classed protected and the component reports the problem).
  Result<std::vector<std::filesystem::path>> user_writable_roots;
  // The owner's override packs folder (platform/packs); empty: no owner packs.
  std::function<Result<std::optional<std::filesystem::path>>()> owner_packs;
};

// The trust signal of running processes (trust spec) for processes.top and processes.explain,
// and the signer lookup of the events component.
//
// start() loads the knowledge base: the kb.json entries of every loaded knowledge pack
// (docs/packs.md), whether or not the pack applies here, since knowing a program that is not
// installed costs nothing. A pack that is rejected is left out and reported as a problem in
// `status`. Signatures are cached by TrustEvaluator until the file
// changes. signer() never verifies on the caller's thread: an image it has not seen is queued
// for a worker thread and answered from the cache on later calls.
class TrustComponent final : public IComponent, public events::ISignerLookup {
 public:
  // `files` must outlive the component.
  TrustComponent(TrustSources sources, const packs::IPackFiles& files);
  TrustComponent(const TrustComponent&) = delete;
  TrustComponent& operator=(const TrustComponent&) = delete;
  TrustComponent(TrustComponent&&) = delete;
  TrustComponent& operator=(TrustComponent&&) = delete;
  ~TrustComponent() override;

  [[nodiscard]] std::string_view name() const override { return "trust"; }
  [[nodiscard]] Result<void> start(const Context& context) override;
  void stop() override;
  [[nodiscard]] ipc::ComponentHealth health() const override;

  // The trust signal of process `pid` created at `created`. Fails when its image cannot be
  // located or read. Thread-safe, after start().
  [[nodiscard]] Result<trust::TrustSignal> process(std::uint32_t pid, WallTime created);
  // The trust signal of the image file `image` (a persistence target). Fails when it cannot be
  // read. Thread-safe, after start().
  [[nodiscard]] Result<trust::TrustSignal> image(const std::filesystem::path& image);

  // events::ISignerLookup. Thread-safe; never blocks on a signature check.
  [[nodiscard]] std::optional<std::string> signer(std::string_view image) override;

 private:
  void run(const std::stop_token& stop);

  TrustSources sources_;
  const packs::IPackFiles* files_;
  std::vector<rules::KbEntry> kb_;  // loaded in start(), read-only afterwards
  std::unique_ptr<trust::TrustEvaluator> evaluator_;
  std::vector<std::string> problems_;  // set in start() only

  std::mutex mutex_;  // guards the members below
  std::condition_variable_any wake_;
  std::deque<std::filesystem::path> queue_;
  std::set<std::string> queued_;                               // by folded path
  std::map<std::string, std::optional<std::string>> signers_;  // by folded path
  std::jthread worker_;
};

// The trust JSON of core/ipc/telemetry.hpp.
[[nodiscard]] nlohmann::json trust_json(const trust::TrustSignal& signal);

}  // namespace myclean::svc
