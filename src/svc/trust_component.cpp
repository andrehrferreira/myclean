#include "svc/trust_component.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "core/ipc/status.hpp"
#include "core/ipc/telemetry.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/packs/loader.hpp"
#include "core/packs/registry.hpp"
#include "core/rules/data_files.hpp"
#include "core/trust/trust.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

namespace {

using Json = nlohmann::json;

// The image's path as a cache key: UTF-8, ASCII letters folded to lower case.
std::string folded(std::string_view image) {
  std::string key(image);
  std::ranges::transform(key, key.begin(), [](char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
  });
  return key;
}

std::string_view status_text(trust::SignatureStatus status) {
  switch (status) {
    case trust::SignatureStatus::valid:
      return ipc::trust_signed;
    case trust::SignatureStatus::none:
      return ipc::trust_unsigned;
    case trust::SignatureStatus::invalid:
      return ipc::trust_invalid;
  }
  return ipc::trust_unsigned;
}

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

}  // namespace

Json trust_json(const trust::TrustSignal& signal) {
  Json reasons = Json::array();
  for (const trust::Reason reason : signal.reasons) {
    reasons.push_back(trust::to_string(reason));
  }
  return Json{
      {"status", status_text(signal.signature.status)},
      {"signer", signal.signature.signer ? Json(*signal.signature.signer) : Json(nullptr)},
      {"path", utf8(signal.image)},
      {"path_class", signal.path_class == trust::PathClass::user_writable ? ipc::path_user_writable
                                                                          : ipc::path_protected},
      {"unrecognized", signal.unrecognized()},
      {"reasons", std::move(reasons)},
      {"description", signal.entry ? Json(signal.entry->description) : Json(nullptr)},
      {"category", signal.entry ? Json(signal.entry->category) : Json(nullptr)}};
}

TrustComponent::TrustComponent(TrustSources sources, const packs::IPackFiles& files)
    : sources_(std::move(sources)), files_(&files) {}

TrustComponent::~TrustComponent() {
  stop();
}

Result<void> TrustComponent::start(const Context& context) {
  const packs::LoadReport report = packs::load_installed(
      *files_, context.data_dir(),
      sources_.owner_packs ? sources_.owner_packs()
                           : Result<std::optional<std::filesystem::path>>(std::nullopt));
  for (const Error& error : report.errors) {
    problems_.push_back("knowledge base: " + error.message);
    spdlog::error("trust: {}", problems_.back());
  }
  kb_ = packs::make_registry(report.packs).kb;
  std::vector<std::filesystem::path> roots;
  if (sources_.user_writable_roots) {
    roots = *sources_.user_writable_roots;
  } else {
    problems_.push_back("user-writable folders unknown, every path is classed protected: " +
                        sources_.user_writable_roots.error().message);
    spdlog::error("trust: {}", problems_.back());
  }
  evaluator_ = std::make_unique<trust::TrustEvaluator>(*sources_.inspector, std::move(roots));
  spdlog::info("trust: {} knowledge-base entries", kb_.size());
  worker_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
  return {};
}

void TrustComponent::stop() {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
}

ipc::ComponentHealth TrustComponent::health() const {
  return ipc::ComponentHealth{
      .name = std::string(name()), .healthy = problems_.empty(), .problems = problems_};
}

Result<trust::TrustSignal> TrustComponent::process(std::uint32_t pid, WallTime created) {
  return sources_.images->image_path(pid, created).and_then([this](const auto& image) {
    return evaluator_->evaluate(image, kb_);
  });
}

std::optional<std::string> TrustComponent::signer(std::string_view image) {
  const std::filesystem::path path(std::u8string(image.begin(), image.end()));
  if (!path.is_absolute()) {
    return std::nullopt;  // a file name only: nothing to verify
  }
  std::string key = folded(image);
  const std::scoped_lock lock(mutex_);
  if (const auto known = signers_.find(key); known != signers_.end()) {
    return known->second;
  }
  if (queued_.insert(std::move(key)).second) {
    queue_.push_back(path);
    wake_.notify_one();
  }
  return std::nullopt;
}

void TrustComponent::run(const std::stop_token& stop) {
  std::unique_lock lock(mutex_);
  while (wake_.wait(lock, stop, [this] { return !queue_.empty(); })) {
    const std::filesystem::path image = std::move(queue_.front());
    queue_.pop_front();
    lock.unlock();
    Result<trust::TrustSignal> signal = evaluator_->evaluate(image, kb_);
    lock.lock();
    std::string key = folded(utf8(image));
    queued_.erase(key);
    signers_.insert_or_assign(std::move(key), signal ? signal->signature.signer : std::nullopt);
  }
}

}  // namespace myclean::svc
