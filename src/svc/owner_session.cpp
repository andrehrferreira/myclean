#include "svc/owner_session.hpp"

#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// clang-format off
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <userenv.h>
#include <wtsapi32.h>
// clang-format on
#include <wil/resource.h>

#include "core/clean/probes.hpp"
#include "core/cleaning/service.hpp"
#include "core/model/result.hpp"
#include "core/safety/deny_list.hpp"
#include "platform/clean/win32_probes.hpp"
#include "platform/fs/protected_locations.hpp"
#include "platform/ipc/security.hpp"
#include "platform/scan/file_content.hpp"

namespace myclean::svc {

namespace {

namespace fs = std::filesystem;

std::wstring upper(std::wstring_view text) {
  std::wstring result(text);
  for (wchar_t& character : result) {
    character = static_cast<wchar_t>(std::towupper(character));
  }
  return result;
}

// An environment block of the owner's token, as the rule engine reads it.
class BlockEnvironment final : public clean::IEnvironment {
 public:
  explicit BlockEnvironment(std::map<std::wstring, std::wstring> values)
      : values_(std::move(values)) {}

  [[nodiscard]] std::optional<std::wstring> variable(std::wstring_view name) const override {
    const auto found = values_.find(upper(name));
    if (found == values_.end()) {
      return std::nullopt;
    }
    const std::wstring& value = found->second;
    const DWORD length = ::GetLongPathNameW(value.c_str(), nullptr, 0);
    if (length > 0) {
      std::wstring buffer(length, L'\0');
      const DWORD written = ::GetLongPathNameW(value.c_str(), buffer.data(), length);
      if (written > 0 && written < length) {
        buffer.resize(written);
        return buffer;
      }
    }
    return value;
  }

 private:
  std::map<std::wstring, std::wstring> values_;  // upper-case names
};

Result<wil::unique_handle> session_token(std::string_view owner_sid) {
  PWTS_SESSION_INFOW sessions = nullptr;
  DWORD count = 0;
  if (::WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count) == FALSE) {
    return fail(Errc::io_error, "WTSEnumerateSessions failed: " + std::to_string(::GetLastError()));
  }
  const auto release = wil::scope_exit([&] { ::WTSFreeMemory(sessions); });
  for (const WTS_SESSION_INFOW& session : std::span(sessions, count)) {
    if (session.State != WTSActive && session.State != WTSDisconnected) {
      continue;
    }
    HANDLE raw = nullptr;
    if (::WTSQueryUserToken(session.SessionId, &raw) == FALSE) {
      continue;
    }
    wil::unique_handle token(raw);
    if (const Result<std::string> sid = platform::token_user_sid(token.get());
        sid && *sid == owner_sid) {
      return token;
    }
  }
  return fail(Errc::not_found, "the owner is not logged on");
}

Result<std::map<std::wstring, std::wstring>> environment_of(HANDLE token) {
  void* block = nullptr;
  if (::CreateEnvironmentBlock(&block, token, FALSE) == FALSE) {
    return fail(Errc::io_error,
                "CreateEnvironmentBlock failed: " + std::to_string(::GetLastError()));
  }
  const auto release = wil::scope_exit([&] { ::DestroyEnvironmentBlock(block); });
  std::map<std::wstring, std::wstring> values;
  for (const auto* entry = static_cast<const wchar_t*>(block); *entry != L'\0';) {
    const std::wstring_view text(entry);
    // A name may start with '=' (the per-drive current folders); the separator follows it.
    if (const std::size_t equals = text.find(L'=', 1); equals != std::wstring_view::npos) {
      values[upper(text.substr(0, equals))] = std::wstring(text.substr(equals + 1));
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): environment block
    entry += text.size() + 1;
  }
  return values;
}

std::optional<fs::path> downloads_of(HANDLE token) {
  wil::unique_cotaskmem_string path;
  if (FAILED(::SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, token, &path))) {
    return std::nullopt;
  }
  return fs::path(path.get());
}

// The volume and file id of `path`, following links: one file reached through a junction (the
// deploy's `current` folder) and through its final path has one identity.
std::optional<FILE_ID_INFO> file_identity(const fs::path& path) {
  const wil::unique_hfile file(::CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  FILE_ID_INFO identity{};
  if (!file || ::GetFileInformationByHandleEx(file.get(), FileIdInfo, &identity,
                                              sizeof(identity)) == FALSE) {
    return std::nullopt;
  }
  return identity;
}

bool same_file(const fs::path& left, const fs::path& right) {
  const std::optional<FILE_ID_INFO> a = file_identity(left);
  const std::optional<FILE_ID_INFO> b = file_identity(right);
  return a && b && a->VolumeSerialNumber == b->VolumeSerialNumber &&
         std::memcmp(a->FileId.Identifier, b->FileId.Identifier, sizeof(a->FileId.Identifier)) == 0;
}

}  // namespace

Result<cleaning::OwnerContext> owner_context(std::string_view owner_sid) {
  const Result<std::string> current = platform::current_user_sid();
  if (!current) {
    return tl::unexpected(current.error());
  }
  if (*current == owner_sid) {
    Result<safety::ProtectedLocations> locations = platform::protected_locations(nullptr);
    if (!locations) {
      return tl::unexpected(locations.error());
    }
    const Result<fs::path> downloads = platform::downloads_folder();
    return cleaning::OwnerContext{
        .environment = std::make_unique<platform::Win32Environment>(),
        .locations = *std::move(locations),
        .downloads = downloads ? std::optional<fs::path>(*downloads) : std::nullopt};
  }
  Result<wil::unique_handle> token = session_token(owner_sid);
  if (!token) {
    return tl::unexpected(token.error());
  }
  Result<std::map<std::wstring, std::wstring>> environment = environment_of(token->get());
  if (!environment) {
    return tl::unexpected(environment.error());
  }
  Result<safety::ProtectedLocations> locations = platform::protected_locations(token->get());
  if (!locations) {
    return tl::unexpected(locations.error());
  }
  return cleaning::OwnerContext{
      .environment = std::make_unique<BlockEnvironment>(*std::move(environment)),
      .locations = *std::move(locations),
      .downloads = downloads_of(token->get())};
}

bool is_owner_tray(std::uint32_t pid, std::string_view owner_sid, const fs::path& tray_image) {
  if (pid == 0 || tray_image.empty()) {
    return false;
  }
  const wil::unique_handle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (!process) {
    return false;
  }
  std::wstring image(UNICODE_STRING_MAX_CHARS, L'\0');
  auto size = static_cast<DWORD>(image.size());
  if (::QueryFullProcessImageNameW(process.get(), 0, image.data(), &size) == FALSE) {
    return false;
  }
  image.resize(size);
  // The image name is the final path, with junctions resolved, while `tray_image` may name the
  // same file through one; so the files are compared, not the texts.
  if (::CompareStringOrdinal(image.c_str(), -1, tray_image.c_str(), -1, TRUE) != CSTR_EQUAL &&
      !same_file(image, tray_image)) {
    return false;
  }
  DWORD session = 0;
  if (::ProcessIdToSessionId(pid, &session) == FALSE || session == 0) {
    return false;
  }
  wil::unique_handle token;
  if (::OpenProcessToken(process.get(), TOKEN_QUERY, &token) == FALSE) {
    return false;
  }
  const Result<std::string> sid = platform::token_user_sid(token.get());
  return sid && *sid == owner_sid;
}

}  // namespace myclean::svc
