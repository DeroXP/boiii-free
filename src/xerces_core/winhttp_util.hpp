#pragma once

// WinHTTP helpers shared by the backend's transports (crash_http.cpp for
// requests, websocket.cpp for the account socket): UTF-8 and UTF-16, a
// handle that closes itself, and the hosts they may reach: the Xerces
// backend (crash_default_base), or this PC for tests. Internal to
// xerces_core.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winhttp.h>

#include <string>
#include <string_view>

#include "crash_report.hpp"

#pragma comment(lib, "winhttp.lib")

namespace xerces_core::winhttp {
inline std::wstring wide(std::string_view text) {
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

inline std::string narrow(const std::wstring &text) {
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

inline std::string last_error(const char *what) {
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

inline bool local_host(const std::wstring &host) {
  return host == L"127.0.0.1" || host == L"localhost" || host == L"::1" ||
         host == L"[::1]";
}

// The Xerces backend's host (crash_default_base without "https://").
inline bool backend_host(const std::wstring &host) {
  std::wstring low = host;
  for (wchar_t &c : low) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
  }
  constexpr std::string_view scheme = "https://";
  return low == wide(crash_default_base.substr(scheme.size()));
}
} // namespace xerces_core::winhttp
