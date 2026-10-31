#include "platform/ipc/security.hpp"

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <sddl.h>
#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"

namespace myclean::platform {

namespace {

// SIDs and SDDL are ASCII, so the conversions are per character.
std::wstring widen(std::string_view text) {
  return {text.begin(), text.end()};
}

std::string narrow(std::wstring_view text) {
  std::string result;
  result.reserve(text.size());
  for (const wchar_t c : text) {
    result.push_back(static_cast<char>(c));
  }
  return result;
}

tl::unexpected<Error> win32_failure(std::string_view action, DWORD error) {
  return fail(error == ERROR_ACCESS_DENIED ? Errc::access_denied : Errc::io_error,
              std::string(action) + ": Win32 error " + std::to_string(error));
}

Result<std::string> sid_string(PSID sid) {
  wil::unique_hlocal_string text;
  if (::ConvertSidToStringSidW(sid, text.put()) == FALSE) {
    return win32_failure("ConvertSidToStringSid", ::GetLastError());
  }
  return narrow(text.get());
}

}  // namespace

Result<std::string> token_user_sid(HANDLE token) {
  DWORD size = 0;
  ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  if (size == 0) {
    return win32_failure("GetTokenInformation(TokenUser)", ::GetLastError());
  }
  // operator new aligns the buffer for TOKEN_USER.
  std::vector<std::byte> buffer(size);
  if (::GetTokenInformation(token, TokenUser, buffer.data(), size, &size) == FALSE) {
    return win32_failure("GetTokenInformation(TokenUser)", ::GetLastError());
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Win32 returns a TOKEN_USER here
  const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
  return sid_string(user->User.Sid);
}

Result<std::string> current_user_sid() {
  return token_user_sid(::GetCurrentProcessToken());
}

Result<void> check_sid(std::string_view sid) {
  wil::unique_hlocal parsed;
  if (::ConvertStringSidToSidW(widen(sid).c_str(), parsed.put()) == FALSE) {
    return fail(Errc::invalid_argument, "not a SID: " + std::string(sid));
  }
  return {};
}

std::string pipe_sddl(std::string_view owner_sid, bool owner_is_server) {
  const std::string owner_rights =
      owner_is_server ? std::string("FA") : std::format("{:#x}", pipe_client_access);
  return std::format("D:P(A;;FA;;;SY)(A;;{};;;{})", owner_rights, owner_sid);
}

Result<wil::unique_hlocal_security_descriptor> make_security_descriptor(std::string_view sddl) {
  wil::unique_hlocal_security_descriptor descriptor;
  if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(widen(sddl).c_str(), SDDL_REVISION_1,
                                                             descriptor.put(), nullptr) == FALSE) {
    return fail(Errc::invalid_argument, "invalid security descriptor " + std::string(sddl) +
                                            ": Win32 error " + std::to_string(::GetLastError()));
  }
  return descriptor;
}

Result<std::vector<AllowedAce>> allowed_aces(PSECURITY_DESCRIPTOR descriptor) {
  BOOL present = FALSE;
  BOOL defaulted = FALSE;
  PACL dacl = nullptr;
  if (::GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) == FALSE) {
    return win32_failure("GetSecurityDescriptorDacl", ::GetLastError());
  }
  if (present == FALSE || dacl == nullptr) {
    return fail(Errc::invalid_data, "the security descriptor has no DACL (everyone has access)");
  }
  ACL_SIZE_INFORMATION size{};
  if (::GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation) == FALSE) {
    return win32_failure("GetAclInformation", ::GetLastError());
  }
  std::vector<AllowedAce> aces;
  for (DWORD index = 0; index < size.AceCount; ++index) {
    void* ace = nullptr;
    if (::GetAce(dacl, index, &ace) == FALSE) {
      return win32_failure("GetAce", ::GetLastError());
    }
    const auto* header = static_cast<const ACE_HEADER*>(ace);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
      return fail(Errc::invalid_data,
                  "ACE " + std::to_string(index) + " is not an access-allowed ACE");
    }
    auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
    Result<std::string> sid = sid_string(&allowed->SidStart);
    if (!sid) {
      return tl::unexpected(sid.error());
    }
    aces.push_back(
        AllowedAce{.sid = std::move(*sid), .mask = allowed->Mask, .flags = header->AceFlags});
  }
  return aces;
}

Result<std::vector<AllowedAce>> object_aces(HANDLE object) {
  DWORD size = 0;
  ::GetKernelObjectSecurity(object, DACL_SECURITY_INFORMATION, nullptr, 0, &size);
  if (size == 0) {
    return win32_failure("GetKernelObjectSecurity", ::GetLastError());
  }
  std::vector<std::byte> buffer(size);
  if (::GetKernelObjectSecurity(object, DACL_SECURITY_INFORMATION, buffer.data(), size, &size) ==
      FALSE) {
    return win32_failure("GetKernelObjectSecurity", ::GetLastError());
  }
  return allowed_aces(buffer.data());
}

}  // namespace myclean::platform
