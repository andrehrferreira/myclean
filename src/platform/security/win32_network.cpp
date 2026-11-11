#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// clang-format off: winsock2.h must precede windows.h.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <wil/resource.h>
// clang-format on

#include "core/model/result.hpp"
#include "core/security/verdicts.hpp"
#include "platform/security/win32_security.hpp"
#include "platform/settings/win32_support.hpp"

namespace myclean::platform {
namespace {

using settings_detail::widen;
using settings_detail::win32_error;

constexpr int timeout_ms = 15'000;
constexpr DWORD status_ok = 200;
constexpr DWORD status_not_found = 404;
constexpr DWORD status_unauthorized = 401;
constexpr DWORD status_forbidden = 403;
constexpr DWORD status_quota = 429;
constexpr std::size_t max_answer = std::size_t{4} * 1024 * 1024;
constexpr std::size_t sha256_hex = 64;

using UniqueInternet =
    wil::unique_any<HINTERNET, decltype(&::WinHttpCloseHandle), ::WinHttpCloseHandle>;

bool hex_digest(std::string_view text) {
  return text.size() == sha256_hex && std::ranges::all_of(text, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

bool key_text(std::string_view key) {
  return !key.empty() && std::ranges::all_of(key, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  });
}
}  // namespace

Result<std::optional<security::HashReport>> Win32HashLookup::lookup(std::string_view sha256,
                                                                    std::string_view api_key) {
  // Nothing but a hex digest and a letters-and-digits key goes into the request.
  if (!hex_digest(sha256) || !key_text(api_key)) {
    return fail(Errc::invalid_argument, "VirusTotal takes a SHA-256 in hex and a plain API key");
  }
  const UniqueInternet session{::WinHttpOpen(L"MyClean", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session) {
    return win32_error("opening WinHTTP", ::GetLastError());
  }
  ::WinHttpSetTimeouts(session.get(), timeout_ms, timeout_ms, timeout_ms, timeout_ms);
  const UniqueInternet connection{
      ::WinHttpConnect(session.get(), L"www.virustotal.com", INTERNET_DEFAULT_HTTPS_PORT, 0)};
  if (!connection) {
    return win32_error("connecting to VirusTotal", ::GetLastError());
  }
  const std::wstring path = L"/api/v3/files/" + widen(sha256);
  const UniqueInternet request{
      ::WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
  if (!request) {
    return win32_error("preparing the VirusTotal request", ::GetLastError());
  }
  const std::wstring header = L"x-apikey: " + widen(api_key);
  if (::WinHttpSendRequest(request.get(), header.c_str(), static_cast<DWORD>(header.size()),
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE ||
      ::WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
    return win32_error("asking VirusTotal", ::GetLastError());
  }
  DWORD status = 0;
  DWORD size = sizeof(status);
  if (::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX) == FALSE) {
    return win32_error("reading VirusTotal's answer", ::GetLastError());
  }
  if (status == status_not_found) {
    return std::optional<security::HashReport>{};
  }
  if (status == status_unauthorized || status == status_forbidden) {
    return fail(Errc::refused, "VirusTotal refused the API key");
  }
  if (status == status_quota) {
    return fail(Errc::refused, "VirusTotal's quota for the API key is used up");
  }
  if (status != status_ok) {
    return fail(Errc::io_error, "VirusTotal answered with HTTP " + std::to_string(status));
  }
  std::string body;
  for (;;) {
    std::array<char, 8192> buffer{};
    DWORD read = 0;
    if (::WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read) ==
        FALSE) {
      return win32_error("reading VirusTotal's answer", ::GetLastError());
    }
    if (read == 0) {
      break;
    }
    body.append(buffer.data(), read);
    if (body.size() > max_answer) {
      return fail(Errc::invalid_data, "VirusTotal's answer is too large");
    }
  }
  Result<security::HashReport> report = security::parse_hash_report(body);
  if (!report) {
    return tl::unexpected(report.error());
  }
  return std::optional<security::HashReport>{*report};
}

Win32Resolver::Win32Resolver() {
  WSADATA data{};
  started_ = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

Win32Resolver::~Win32Resolver() {
  if (started_) {
    ::WSACleanup();
  }
}

std::optional<std::string> Win32Resolver::name(std::string_view address) {
  if (!started_) {
    return std::nullopt;
  }
  const std::string text(address);
  sockaddr_storage storage{};
  int length = 0;
  auto* v4 = reinterpret_cast<sockaddr_in*>(&storage);   // NOLINT(*-reinterpret-cast): sockaddr
  auto* v6 = reinterpret_cast<sockaddr_in6*>(&storage);  // NOLINT(*-reinterpret-cast): sockaddr
  if (::inet_pton(AF_INET, text.c_str(), &v4->sin_addr) == 1) {
    v4->sin_family = AF_INET;
    length = sizeof(sockaddr_in);
  } else if (::inet_pton(AF_INET6, text.c_str(), &v6->sin6_addr) == 1) {
    v6->sin6_family = AF_INET6;
    length = sizeof(sockaddr_in6);
  } else {
    return std::nullopt;
  }
  std::array<char, NI_MAXHOST> host{};
  if (::getnameinfo(reinterpret_cast<const sockaddr*>(&storage),  // NOLINT(*-reinterpret-cast)
                    length, host.data(), static_cast<DWORD>(host.size()), nullptr, 0,
                    NI_NAMEREQD) != 0) {
    return std::nullopt;
  }
  return std::string(host.data());
}

}  // namespace myclean::platform
