#include "platform/service/system.hpp"

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

#include <wil/resource.h>
// clang-format off
#include <windows.h>  // first: the headers below need its types
#include <KnownFolders.h>
#include <ShlObj.h>
#include <aclapi.h>
#include <sddl.h>
#include <wtsapi32.h>
// clang-format on

#include "core/model/result.hpp"
#include "platform/ipc/pipe_io.hpp"
#include "platform/ipc/security.hpp"

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;

Result<fs::path> known_folder(REFKNOWNFOLDERID id, std::string_view name) {
  wil::unique_cotaskmem_string path;
  if (const HRESULT found = ::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, path.put());
      FAILED(found)) {
    return fail(Errc::not_found,
                "known folder " + std::string(name) + ": HRESULT " + std::to_string(found));
  }
  return fs::path(path.get());
}

}  // namespace

Result<fs::path> program_data_dir() {
  return known_folder(FOLDERID_ProgramData, "ProgramData");
}

Result<fs::path> local_app_data_dir() {
  return known_folder(FOLDERID_LocalAppData, "LocalAppData");
}

Result<fs::path> executable_path() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length =
        ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) {
      return pipe_io::failure("GetModuleFileName", ::GetLastError());
    }
    if (length < path.size()) {
      path.resize(length);
      return fs::path(path);
    }
    path.resize(path.size() * 2);
  }
}

Result<std::string> console_user_sid() {
  const DWORD session = ::WTSGetActiveConsoleSessionId();
  if (session == 0xFFFFFFFF) {
    return fail(Errc::not_found, "no session is attached to the console");
  }
  wil::unique_handle token;
  if (::WTSQueryUserToken(session, token.put()) == FALSE) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_NO_TOKEN) {
      return fail(Errc::not_found, "nobody is logged on at the console");
    }
    return fail(error == ERROR_PRIVILEGE_NOT_HELD ? Errc::access_denied : Errc::io_error,
                "WTSQueryUserToken: Win32 error " + std::to_string(error));
  }
  return token_user_sid(token.get());
}

Result<std::string> file_owner_sid(const fs::path& path) {
  PSID owner = nullptr;
  wil::unique_hlocal_security_descriptor descriptor;
  if (const DWORD error =
          ::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
                                  nullptr, nullptr, nullptr, descriptor.put());
      error != ERROR_SUCCESS) {
    return pipe_io::failure("GetNamedSecurityInfo " + pipe_io::utf8(path.native()), error);
  }
  wil::unique_hlocal_string text;
  if (::ConvertSidToStringSidW(owner, text.put()) == FALSE) {
    return pipe_io::failure("ConvertSidToStringSid", ::GetLastError());
  }
  return pipe_io::utf8(text.get());
}

std::string state_dir_sddl(std::string_view owner_sid) {
  return std::format("O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;{})", owner_sid);
}

Result<void> protect_state_dir(const fs::path& dir, std::string_view owner_sid) {
  Result<wil::unique_hlocal_security_descriptor> descriptor =
      make_security_descriptor(state_dir_sddl(owner_sid));
  if (!descriptor) {
    return tl::unexpected(descriptor.error());
  }
  PSID owner = nullptr;
  BOOL owner_defaulted = FALSE;
  BOOL present = FALSE;
  BOOL dacl_defaulted = FALSE;
  PACL dacl = nullptr;
  if (::GetSecurityDescriptorOwner(descriptor->get(), &owner, &owner_defaulted) == FALSE ||
      ::GetSecurityDescriptorDacl(descriptor->get(), &present, &dacl, &dacl_defaulted) == FALSE) {
    return pipe_io::failure("GetSecurityDescriptor", ::GetLastError());
  }
  std::wstring path = dir.native();
  // Reset: every entry below gets the owner and only the inherited ACEs, so nothing created
  // there before (by another user, before the first start) keeps rights of its own.
  if (const DWORD error =
          ::TreeSetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT,
                                      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                          PROTECTED_DACL_SECURITY_INFORMATION,
                                      owner, nullptr, dacl, nullptr, TREE_SEC_INFO_RESET, nullptr,
                                      ProgressInvokeNever, nullptr);
      error != ERROR_SUCCESS) {
    return pipe_io::failure("TreeSetNamedSecurityInfo " + pipe_io::utf8(path), error);
  }
  return {};
}

}  // namespace myclean::platform
