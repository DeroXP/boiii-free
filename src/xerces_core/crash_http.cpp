// The crash reports' HTTP transport (crash_outbox.hpp): WinHTTP, a system
// DLL, so no TLS or HTTP library ships with Xerces for it. HTTPS uses the
// system's certificate store; plain HTTP is allowed to local addresses only,
// for tests against a backend on this PC (crash_base_url_from).

#include "crash_outbox.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winhttp.h>

#include <cstdlib>

#pragma comment(lib, "winhttp.lib")

namespace xerces_core {
namespace {
std::wstring wide(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(
      CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), size);
  return out;
}

std::string narrow(const std::wstring &text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr,
                                       0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), size, nullptr, nullptr);
  return out;
}

std::string last_error(const char *what) {
  return std::string(what) + " failed (" + std::to_string(GetLastError()) + ")";
}

struct handle {
  HINTERNET value = nullptr;
  explicit handle(HINTERNET h) : value(h) {}
  ~handle() {
    if (value) {
      WinHttpCloseHandle(value);
    }
  }
  handle(const handle &) = delete;
  handle &operator=(const handle &) = delete;
  explicit operator bool() const { return value != nullptr; }
};

bool local_host(const std::wstring &host) {
  return host == L"127.0.0.1" || host == L"localhost" || host == L"::1" ||
         host == L"[::1]";
}
} // namespace

http_response winhttp_send(const http_request &request) {
  http_response out;
  const std::wstring url = wide(request.url);
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256]{};
  wchar_t path[2048]{};
  parts.lpszHostName = host;
  parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
    out.error = last_error("WinHttpCrackUrl");
    return out;
  }
  const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
  if (!secure && !local_host(host)) {
    out.error = "plain http is only for local addresses";
    return out;
  }

  std::wstring agent = L"Xerces";
  std::wstring headers;
  for (const auto &[name, value] : request.headers) {
    if (name == "User-Agent") {
      agent = wide(value);
      continue;
    }
    headers += wide(name) + L": " + wide(value) + L"\r\n";
  }

  const handle session(
      WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) {
    out.error = last_error("WinHttpOpen");
    return out;
  }
  const int timeout = request.timeout_ms > 0 ? request.timeout_ms : 15000;
  WinHttpSetTimeouts(session.value, timeout, timeout, timeout, timeout);
  if (secure) {
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 |
                      WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS,
                          &protocols, sizeof(protocols))) {
      // Windows 10 before 1903 knows no TLS 1.3 flag: TLS 1.2 alone.
      protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
      WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS,
                       &protocols, sizeof(protocols));
    }
  }
  const handle connection(WinHttpConnect(session.value, host, parts.nPort, 0));
  if (!connection) {
    out.error = last_error("WinHttpConnect");
    return out;
  }
  const std::wstring method = wide(request.method);
  const handle req(WinHttpOpenRequest(
      connection.value, method.c_str(), path, nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
  if (!req) {
    out.error = last_error("WinHttpOpenRequest");
    return out;
  }
  // Redirects are not followed: a report goes to the host named, or nowhere.
  DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  WinHttpSetOption(req.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects,
                   sizeof(redirects));
  const BOOL sent = WinHttpSendRequest(
      req.value,
      headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
      headers.empty() ? 0 : static_cast<DWORD>(-1L),
      request.body.empty() ? WINHTTP_NO_REQUEST_DATA
                           : const_cast<char *>(request.body.data()),
      static_cast<DWORD>(request.body.size()),
      static_cast<DWORD>(request.body.size()), 0);
  if (!sent) {
    out.error = last_error("WinHttpSendRequest");
    return out;
  }
  if (!WinHttpReceiveResponse(req.value, nullptr)) {
    out.error = last_error("WinHttpReceiveResponse");
    return out;
  }
  DWORD status = 0;
  DWORD size = sizeof(status);
  if (!WinHttpQueryHeaders(
          req.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
          WINHTTP_NO_HEADER_INDEX)) {
    out.error = last_error("WinHttpQueryHeaders");
    return out;
  }
  out.reached = true;
  out.status = static_cast<int>(status);
  wchar_t retry[64]{};
  DWORD retry_size = sizeof(retry);
  if (WinHttpQueryHeaders(req.value, WINHTTP_QUERY_RETRY_AFTER,
                          WINHTTP_HEADER_NAME_BY_INDEX, retry, &retry_size,
                          WINHTTP_NO_HEADER_INDEX)) {
    const int seconds = std::atoi(narrow(retry).c_str());
    if (seconds > 0) {
      out.retry_after = seconds;
    }
  }
  // The answer is a small JSON object; anything past 64 KiB is not read.
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(req.value, &available) || available == 0) {
      break;
    }
    std::string chunk(available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(req.value, chunk.data(), available, &read) ||
        read == 0) {
      break;
    }
    out.body.append(chunk.data(), read);
    if (out.body.size() > 64 * 1024) {
      break;
    }
  }
  return out;
}
} // namespace xerces_core
