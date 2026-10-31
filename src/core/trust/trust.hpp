#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/rules/schema.hpp"

// The trust signal of a process image (trust spec, architecture F-036): its Authenticode
// signature, signer, path class and knowledge-base entry, and whether it is unrecognized. The
// signal reports facts only; it never calls an image dangerous or malicious.
namespace myclean::trust {

enum class SignatureStatus {
  valid,    // "signed": embedded or Windows-catalog signature that verifies
  none,     // "unsigned": no embedded signature and no catalog entry
  invalid,  // "invalid": a signature that does not verify (modified bytes, untrusted chain, ...)
};

[[nodiscard]] std::string_view to_string(SignatureStatus status);

enum class PathClass {
  protected_location,  // "protected": a standard user cannot write there
  user_writable,       // "user-writable": %TEMP%, AppData, Downloads, ... (a user profile)
};

[[nodiscard]] std::string_view to_string(PathClass path_class);

// Why an image is unrecognized. not_in_knowledge_base is always the first reason.
enum class Reason {
  not_in_knowledge_base,  // "not-in-knowledge-base"
  unsigned_image,         // "unsigned"
  invalid_signature,      // "invalid-signature"
  unlisted_signer,        // "signer-not-listed": valid signature, signer no KB entry names
  user_writable_path,     // "user-writable-path"
};

[[nodiscard]] std::string_view to_string(Reason reason);

struct Signature {
  SignatureStatus status = SignatureStatus::none;
  std::optional<std::string> signer;  // the signing certificate's name; set only when valid

  friend bool operator==(const Signature&, const Signature&) = default;
};

// What identifies one version of a file cheaply: a changed file gets a new stamp.
struct FileStamp {
  std::uint64_t size = 0;
  WallTime modified;

  friend bool operator==(const FileStamp&, const FileStamp&) = default;
};

// The facts about an image file that only the OS can supply. Implemented by
// src/platform/trust; tests use fakes.
class IImageInspector {
 public:
  virtual ~IImageInspector() = default;
  IImageInspector(const IImageInspector&) = delete;
  IImageInspector& operator=(const IImageInspector&) = delete;
  IImageInspector(IImageInspector&&) = delete;
  IImageInspector& operator=(IImageInspector&&) = delete;

  // Size and last-write time; cheap, read on every evaluation.
  [[nodiscard]] virtual Result<FileStamp> stamp(const std::filesystem::path& image) const = 0;
  // The Authenticode result, embedded signature or catalog. Reads the whole file; never
  // touches the network.
  [[nodiscard]] virtual Result<Signature> verify(const std::filesystem::path& image) const = 0;

 protected:
  IImageInspector() = default;
};

struct TrustSignal {
  std::filesystem::path image;
  Signature signature;
  PathClass path_class = PathClass::protected_location;
  std::optional<rules::KbEntry> entry;  // the matching knowledge-base entry
  std::vector<Reason> reasons;          // empty unless unrecognized

  [[nodiscard]] bool unrecognized() const { return !reasons.empty(); }
};

// user_writable when `image` lies inside one of `roots`, compared without regard to ASCII case.
[[nodiscard]] PathClass classify_path(const std::filesystem::path& image,
                                      std::span<const std::filesystem::path> roots);

// The first entry that matches `image`: same file name (ignoring ASCII case), the same signer
// when the entry names one (so an unsigned look-alike never matches), and the entry's path
// pattern found in the full path when it has one. Null when none matches.
[[nodiscard]] const rules::KbEntry* match_entry(std::span<const rules::KbEntry> kb,
                                                const std::filesystem::path& image,
                                                const std::optional<std::string>& signer);

// The signal from the facts. Unrecognized if and only if no entry matches and at least one of:
// unsigned or invalid signature, a signer that no entry names, a user-writable path.
[[nodiscard]] TrustSignal assess(const std::filesystem::path& image, const Signature& signature,
                                 PathClass path_class, std::span<const rules::KbEntry> kb);

// Computes trust signals, caching each image's signature by path until its stamp changes.
// Thread-safe.
class TrustEvaluator {
 public:
  // `inspector` must outlive the evaluator. `user_writable_roots`: see classify_path.
  TrustEvaluator(const IImageInspector& inspector,
                 std::vector<std::filesystem::path> user_writable_roots);

  // Fails when the image cannot be read.
  [[nodiscard]] Result<TrustSignal> evaluate(const std::filesystem::path& image,
                                             std::span<const rules::KbEntry> kb);

 private:
  struct Cached {
    FileStamp stamp;
    Signature signature;
  };

  [[nodiscard]] Result<Signature> signature(const std::filesystem::path& image);

  const IImageInspector* inspector_;
  std::vector<std::filesystem::path> roots_;
  std::mutex mutex_;                     // guards cache_
  std::map<std::string, Cached> cache_;  // by path folded to lower case
};

}  // namespace myclean::trust
