#include "core/trust/trust.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <mutex>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/enum_names.hpp"
#include "core/model/result.hpp"
#include "core/rules/schema.hpp"

namespace myclean::trust {

namespace {

constexpr std::array<std::string_view, 3> signature_names = {"signed", "unsigned", "invalid"};
constexpr std::array<std::string_view, 2> path_class_names = {"protected", "user-writable"};
constexpr std::array<std::string_view, 5> reason_names = {"not-in-knowledge-base", "unsigned",
                                                          "invalid-signature", "signer-not-listed",
                                                          "user-writable-path"};

char fold(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string folded(std::string_view text) {
  std::string result(text);
  std::ranges::transform(result, result.begin(), fold);
  return result;
}

bool equal_folded(std::string_view left, std::string_view right) {
  return std::ranges::equal(left, right, {}, fold, fold);
}

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

// The normalized, case-folded path with backslash separators and no trailing separator.
std::string path_key(const std::filesystem::path& path) {
  std::string key = folded(utf8(path.lexically_normal().make_preferred()));
  while (key.size() > 1 && (key.back() == '\\' || key.back() == '/')) {
    key.pop_back();
  }
  return key;
}

}  // namespace

std::string_view to_string(SignatureStatus status) {
  return detail::enum_name(signature_names, status);
}

std::string_view to_string(PathClass path_class) {
  return detail::enum_name(path_class_names, path_class);
}

std::string_view to_string(Reason reason) {
  return detail::enum_name(reason_names, reason);
}

PathClass classify_path(const std::filesystem::path& image,
                        std::span<const std::filesystem::path> roots) {
  const std::string key = path_key(image);
  const bool inside = std::ranges::any_of(roots, [&key](const std::filesystem::path& root) {
    const std::string prefix = path_key(root);
    return !prefix.empty() && key.starts_with(prefix) &&
           (key.size() == prefix.size() || key[prefix.size()] == '\\');
  });
  return inside ? PathClass::user_writable : PathClass::protected_location;
}

const rules::KbEntry* match_entry(std::span<const rules::KbEntry> kb,
                                  const std::filesystem::path& image,
                                  const std::optional<std::string>& signer) {
  const std::string name = utf8(image.filename());
  const std::string full = utf8(image);
  const auto entry = std::ranges::find_if(kb, [&](const rules::KbEntry& candidate) {
    return equal_folded(candidate.image, name) &&
           (!candidate.signer || (signer && equal_folded(*candidate.signer, *signer))) &&
           (!candidate.path || std::regex_search(full, candidate.path->regex));
  });
  return entry == kb.end() ? nullptr : &*entry;
}

TrustSignal assess(const std::filesystem::path& image, const Signature& signature,
                   PathClass path_class, std::span<const rules::KbEntry> kb) {
  TrustSignal signal{.image = image, .signature = signature, .path_class = path_class};
  if (const rules::KbEntry* entry = match_entry(kb, image, signature.signer)) {
    signal.entry = *entry;
    return signal;
  }
  std::vector<Reason> reasons;
  if (signature.status == SignatureStatus::none) {
    reasons.push_back(Reason::unsigned_image);
  } else if (signature.status == SignatureStatus::invalid) {
    reasons.push_back(Reason::invalid_signature);
  } else if (!signature.signer ||
             std::ranges::none_of(kb, [&signature](const rules::KbEntry& entry) {
               return entry.signer && equal_folded(*entry.signer, *signature.signer);
             })) {
    reasons.push_back(Reason::unlisted_signer);
  }
  if (path_class == PathClass::user_writable) {
    reasons.push_back(Reason::user_writable_path);
  }
  if (!reasons.empty()) {
    signal.reasons.push_back(Reason::not_in_knowledge_base);
    signal.reasons.insert(signal.reasons.end(), reasons.begin(), reasons.end());
  }
  return signal;
}

TrustEvaluator::TrustEvaluator(const IImageInspector& inspector,
                               std::vector<std::filesystem::path> user_writable_roots)
    : inspector_(&inspector), roots_(std::move(user_writable_roots)) {}

Result<TrustSignal> TrustEvaluator::evaluate(const std::filesystem::path& image,
                                             std::span<const rules::KbEntry> kb) {
  return signature(image).transform([&](const Signature& signature) {
    return assess(image, signature, classify_path(image, roots_), kb);
  });
}

Result<Signature> TrustEvaluator::signature(const std::filesystem::path& image) {
  Result<FileStamp> stamp = inspector_->stamp(image);
  if (!stamp) {
    return tl::unexpected(std::move(stamp.error()));
  }
  const std::string key = path_key(image);
  {
    const std::scoped_lock lock(mutex_);
    if (const auto cached = cache_.find(key);
        cached != cache_.end() && cached->second.stamp == *stamp) {
      return cached->second.signature;
    }
  }
  // Verified outside the lock: it reads the whole file. The stamp is the one read before, so a
  // file changed meanwhile is verified again on the next evaluation.
  Result<Signature> verified = inspector_->verify(image);
  if (verified) {
    const std::scoped_lock lock(mutex_);
    cache_.insert_or_assign(key, Cached{.stamp = *stamp, .signature = *verified});
  }
  return verified;
}

}  // namespace myclean::trust
