#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/clean/probes.hpp"
#include "core/packs/schema.hpp"

// Whether a pack applies to this machine: its manifest's applies_when.
namespace myclean::packs {

// What the conditions ask the machine. Implemented by the platform layer (which may cache the
// answers for one load) and by fakes in tests.
class IMachine {
 public:
  virtual ~IMachine() = default;
  IMachine(const IMachine&) = delete;
  IMachine& operator=(const IMachine&) = delete;
  IMachine(IMachine&&) = delete;
  IMachine& operator=(IMachine&&) = delete;

  // The DisplayName of every installed program (the Uninstall keys of HKLM, its 32-bit view and
  // HKCU) and the name of every installed Appx package ("PythonSoftwareFoundation.Python.3.12").
  [[nodiscard]] virtual std::vector<std::string> installed_apps() const = 0;
  // The image name of every running process ("ollama.exe").
  [[nodiscard]] virtual std::vector<std::string> running_images() const = 0;
  // The Windows build number (26100).
  [[nodiscard]] virtual std::uint32_t os_build() const = 0;

 protected:
  IMachine() = default;
};

// Everything applies() reads. Every pointer must outlive the call.
struct Probes {
  const IMachine* machine = nullptr;
  const clean::IFileTree* tree = nullptr;  // path_exists, never through a reparse point
  const clean::IEnvironment* environment = nullptr;
};

// True when every `all` condition holds and, when `any` is not empty, at least one `any`
// condition does.
[[nodiscard]] bool applies(const Applicability& applicability, const Probes& probes);

}  // namespace myclean::packs
