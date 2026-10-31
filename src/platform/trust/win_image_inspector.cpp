#include "platform/trust/win_image_inspector.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

// clang-format off
#include <windows.h>  // first: the other Win32 headers need its types
#include <KnownFolders.h>
#include <ShlObj.h>
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>
#include <wil/resource.h>  // last: its COM helpers exist only when objbase.h came first
// clang-format on

#include "core/model/result.hpp"
#include "core/trust/trust.hpp"

namespace myclean::platform {

namespace {

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

Error error_of(DWORD code, const std::filesystem::path& path, const char* what) {
  Errc errc = Errc::io_error;
  if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
    errc = Errc::not_found;
  } else if (code == ERROR_ACCESS_DENIED) {
    errc = Errc::access_denied;
  } else if (code == ERROR_SHARING_VIOLATION) {
    errc = Errc::in_use;
  }
  return Error{.code = errc,
               .message = utf8(path) + ": " + what + ": " +
                          std::system_category().message(static_cast<int>(code))};
}

// The display name (CN) of the leaf certificate of the first signer, if any.
std::optional<std::string> signer_of(HANDLE state) {
  CRYPT_PROVIDER_DATA* provider = ::WTHelperProvDataFromStateData(state);
  CRYPT_PROVIDER_SGNR* signer =
      provider == nullptr ? nullptr : ::WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0);
  CRYPT_PROVIDER_CERT* leaf =
      signer == nullptr ? nullptr : ::WTHelperGetProvCertFromChain(signer, 0);
  if (leaf == nullptr || leaf->pCert == nullptr) {
    return std::nullopt;
  }
  const DWORD length =
      ::CertGetNameStringW(leaf->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
  if (length <= 1) {
    return std::nullopt;
  }
  std::wstring name(length, L'\0');
  ::CertGetNameStringW(leaf->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name.data(), length);
  name.resize(length - 1);
  return utf8(name);
}

struct Verdict {
  LONG status = 0;
  DWORD last_error = 0;  // the detail of TRUST_E_NOSIGNATURE
  std::optional<std::string> signer;
};

// WinVerifyTrust over `data`, whose subject (file or catalog member) is already set. No UI, no
// revocation check, no URL retrieval beyond the local cache.
Verdict verify_trust(WINTRUST_DATA& data) {
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  data.cbStruct = sizeof(data);
  data.dwUIChoice = WTD_UI_NONE;
  data.fdwRevocationChecks = WTD_REVOKE_NONE;
  data.dwStateAction = WTD_STATEACTION_VERIFY;
  data.dwProvFlags = WTD_REVOCATION_CHECK_NONE | WTD_CACHE_ONLY_URL_RETRIEVAL;
  // INVALID_HANDLE_VALUE: no interactive user, so WinVerifyTrust never shows anything.
  auto* const no_window = static_cast<HWND>(INVALID_HANDLE_VALUE);
  Verdict verdict{.status = ::WinVerifyTrust(no_window, &action, &data)};
  verdict.last_error = ::GetLastError();
  if (verdict.status == ERROR_SUCCESS) {
    verdict.signer = signer_of(data.hWVTStateData);
  }
  data.dwStateAction = WTD_STATEACTION_CLOSE;
  ::WinVerifyTrust(no_window, &action, &data);
  return verdict;
}

trust::Signature signature_of(Verdict verdict) {
  if (verdict.status == ERROR_SUCCESS) {
    return {.status = trust::SignatureStatus::valid, .signer = std::move(verdict.signer)};
  }
  return {.status = trust::SignatureStatus::invalid, .signer = std::nullopt};
}

// Whether the embedded check found no signature at all, as opposed to one that fails.
bool has_no_signature(const Verdict& verdict) {
  if (verdict.status == TRUST_E_NOSIGNATURE) {
    const auto detail = static_cast<HRESULT>(verdict.last_error);
    return detail == TRUST_E_NOSIGNATURE || detail == TRUST_E_SUBJECT_FORM_UNKNOWN ||
           detail == TRUST_E_PROVIDER_UNKNOWN;
  }
  return verdict.status == TRUST_E_SUBJECT_FORM_UNKNOWN ||
         verdict.status == TRUST_E_PROVIDER_UNKNOWN;
}

std::wstring hex(const std::vector<BYTE>& bytes) {
  std::wstring text;
  for (const BYTE byte : bytes) {
    text += std::format(L"{:02X}", byte);
  }
  return text;
}

// The catalog signature of `file`, or nullopt when no catalog of the system lists its hash
// computed with `algorithm`.
std::optional<trust::Signature> catalog_signature(HANDLE file, const std::filesystem::path& path,
                                                  const wchar_t* algorithm) {
  HCATADMIN admin = nullptr;
  if (::CryptCATAdminAcquireContext2(&admin, nullptr, algorithm, nullptr, 0) == FALSE) {
    return std::nullopt;
  }
  const auto release_admin = wil::scope_exit([admin] { ::CryptCATAdminReleaseContext(admin, 0); });
  DWORD size = 0;
  ::CryptCATAdminCalcHashFromFileHandle2(admin, file, &size, nullptr, 0);
  std::vector<BYTE> hash(size);
  if (size == 0 ||
      ::CryptCATAdminCalcHashFromFileHandle2(admin, file, &size, hash.data(), 0) == FALSE) {
    return std::nullopt;
  }
  HCATINFO catalog = ::CryptCATAdminEnumCatalogFromHash(admin, hash.data(), size, 0, nullptr);
  if (catalog == nullptr) {
    return std::nullopt;
  }
  const auto release_catalog = wil::scope_exit(
      [admin, catalog] { ::CryptCATAdminReleaseCatalogContext(admin, catalog, 0); });
  CATALOG_INFO info{.cbStruct = sizeof(CATALOG_INFO), .wszCatalogFile = {}};
  if (::CryptCATCatalogInfoFromContext(catalog, &info, 0) == FALSE) {
    return std::nullopt;
  }
  const std::wstring tag = hex(hash);
  WINTRUST_CATALOG_INFO member{};
  member.cbStruct = sizeof(member);
  member.pcwszCatalogFilePath = static_cast<const wchar_t*>(info.wszCatalogFile);
  member.pcwszMemberTag = tag.c_str();
  member.pcwszMemberFilePath = path.c_str();
  member.hMemberFile = file;
  member.pbCalculatedFileHash = hash.data();
  member.cbCalculatedFileHash = size;
  member.hCatAdmin = admin;
  WINTRUST_DATA data{};
  data.dwUnionChoice = WTD_CHOICE_CATALOG;
  data.pCatalog = &member;  // NOLINT(cppcoreguidelines-pro-type-union-access): the choice above
  return signature_of(verify_trust(data));
}

Result<std::filesystem::path> known_folder(const KNOWNFOLDERID& id) {
  wil::unique_cotaskmem_string text;
  const HRESULT result = ::SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &text);
  if (FAILED(result)) {
    return fail(Errc::io_error,
                "SHGetKnownFolderPath failed: " + std::system_category().message(result));
  }
  return std::filesystem::path{text.get()};
}

}  // namespace

Result<trust::FileStamp> WinImageInspector::stamp(const std::filesystem::path& image) const {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (::GetFileAttributesExW(image.c_str(), GetFileExInfoStandard, &data) == FALSE) {
    return tl::unexpected(error_of(::GetLastError(), image, "cannot read its attributes"));
  }
  const std::chrono::file_clock::duration since_1601{
      (static_cast<std::int64_t>(data.ftLastWriteTime.dwHighDateTime) << 32U) |
      data.ftLastWriteTime.dwLowDateTime};
  return trust::FileStamp{
      .size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32U) | data.nFileSizeLow,
      .modified = std::chrono::time_point_cast<WallTime::duration>(
          std::chrono::clock_cast<std::chrono::system_clock>(
              std::chrono::file_clock::time_point{since_1601}))};
}

Result<trust::Signature> WinImageInspector::verify(const std::filesystem::path& image) const {
  const wil::unique_hfile file{::CreateFileW(
      image.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (!file) {
    return tl::unexpected(error_of(::GetLastError(), image, "cannot be opened"));
  }
  WINTRUST_FILE_INFO subject{};
  subject.cbStruct = sizeof(subject);
  subject.pcwszFilePath = image.c_str();
  subject.hFile = file.get();
  WINTRUST_DATA data{};
  data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &subject;  // NOLINT(cppcoreguidelines-pro-type-union-access): the choice above
  Verdict embedded = verify_trust(data);
  if (!has_no_signature(embedded)) {
    return signature_of(std::move(embedded));
  }
  for (const wchar_t* algorithm : {L"SHA256", L"SHA1"}) {
    if (std::optional<trust::Signature> signature =
            catalog_signature(file.get(), image, algorithm)) {
      return *std::move(signature);
    }
  }
  return trust::Signature{.status = trust::SignatureStatus::none, .signer = std::nullopt};
}

Result<std::vector<std::filesystem::path>> user_writable_roots() {
  const Result<std::filesystem::path> windows = known_folder(FOLDERID_Windows);
  const Result<std::filesystem::path> profiles = known_folder(FOLDERID_UserProfiles);
  if (!windows || !profiles) {
    return tl::unexpected(windows ? profiles.error() : windows.error());
  }
  std::vector<std::filesystem::path> roots{*windows / L"Temp"};
  // Every profile folder; the junctions "All Users" and "Default User" point elsewhere.
  std::error_code error;
  for (std::filesystem::directory_iterator entry(*profiles, error), end; !error && entry != end;
       entry.increment(error)) {
    std::error_code ignored;
    if (entry->symlink_status(ignored).type() == std::filesystem::file_type::directory) {
      roots.push_back(entry->path());
    }
  }
  if (error) {
    return fail(Errc::io_error, utf8(*profiles) + ": cannot be listed: " + error.message());
  }
  return roots;
}

}  // namespace myclean::platform
