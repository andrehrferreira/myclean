#pragma once

#include <filesystem>
#include <stop_token>
#include <string_view>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::cleanup {

// The Windows disk-cleanup handlers (IEmptyVolumeCache), each named by its key under
// HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\VolumeCaches ("Temporary Files").
// Implemented by platform/cleanup (Win32HandlerHost) and by test fakes.
//
// Both calls fail with refused when the handler shows a window or `stop` is requested,
// access_denied when it needs rights the caller lacks, io_error when it fails or exceeds the
// host's timeout, and not_found when no handler has that name.
class IHandlerHost {
 public:
  virtual ~IHandlerHost() = default;
  IHandlerHost(const IHandlerHost&) = delete;
  IHandlerHost& operator=(const IHandlerHost&) = delete;
  IHandlerHost(IHandlerHost&&) = delete;
  IHandlerHost& operator=(IHandlerHost&&) = delete;

  // What the handler could free on `volume` ("C:\"): Initialize and GetSpaceUsed only.
  [[nodiscard]] virtual Result<Bytes> space_used(std::string_view handler,
                                                 const std::filesystem::path& volume,
                                                 std::stop_token stop) = 0;

  // Lets the handler remove what it found on `volume`: Initialize, GetSpaceUsed, Purge,
  // Deactivate.
  [[nodiscard]] virtual Result<void> purge(std::string_view handler,
                                           const std::filesystem::path& volume,
                                           std::stop_token stop) = 0;

 protected:
  IHandlerHost() = default;
};

}  // namespace myclean::cleanup
