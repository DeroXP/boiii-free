// websocket.hpp: WinHTTP's WebSocket (Windows 8 and later). The upgrade is
// an ordinary GET with WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET; then
// WinHttpWebSocketReceive and WinHttpWebSocketSend work on the socket
// handle, one receive and one send at a time, which the mutexes keep.

#include "websocket.hpp"

#include <atomic>
#include <utility>

#include "winhttp_util.hpp"

namespace xerces_core {
using winhttp::backend_host;
using winhttp::last_error;
using winhttp::local_host;
using winhttp::wide;

struct websocket::state {
  std::mutex handles; // session, connection and socket below
  HINTERNET session = nullptr;
  HINTERNET connection = nullptr;
  HINTERNET socket = nullptr;
  std::mutex sending;
  std::mutex receiving;
  std::atomic<bool> closed{true};

  HINTERNET current_socket() {
    std::scoped_lock lock(handles);
    return closed ? nullptr : socket;
  }

  void close_all() {
    std::scoped_lock lock(handles);
    closed = true;
    // Closing the socket handle ends a receive that is waiting on it.
    for (HINTERNET *h : {&socket, &connection, &session}) {
      if (*h) {
        WinHttpCloseHandle(*h);
        *h = nullptr;
      }
    }
  }
};

websocket::websocket() : state_(std::make_unique<state>()) {}

websocket::~websocket() { state_->close_all(); }

bool websocket::open() const { return !state_->closed; }

bool websocket::connect(std::string_view url, std::string *error,
                        const int timeout_ms) {
  close();
  auto fail = [&](std::string why) {
    if (error) {
      *error = std::move(why);
    }
    return false;
  };

  std::string http_url(url);
  bool secure = false;
  if (http_url.rfind("wss://", 0) == 0) {
    http_url = "https://" + http_url.substr(6);
    secure = true;
  } else if (http_url.rfind("ws://", 0) == 0) {
    http_url = "http://" + http_url.substr(5);
  } else {
    return fail("not a ws:// or wss:// address");
  }
  const std::wstring wurl = wide(http_url);
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256]{};
  wchar_t path[2048]{};
  parts.lpszHostName = host;
  parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
    return fail(last_error("WinHttpCrackUrl"));
  }
  if (!local_host(host) && !backend_host(host)) {
    return fail(
        "the account socket goes to the Xerces backend or this PC only");
  }
  if (!secure && !local_host(host)) {
    return fail("plain ws is only for local addresses");
  }

  winhttp::handle session(
      WinHttpOpen(L"Xerces", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) {
    return fail(last_error("WinHttpOpen"));
  }
  WinHttpSetTimeouts(session.value, timeout_ms, timeout_ms, timeout_ms,
                     timeout_ms);
  if (secure) {
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 |
                      WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS,
                          &protocols, sizeof(protocols))) {
      protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
      WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS,
                       &protocols, sizeof(protocols));
    }
  }
  winhttp::handle connection(
      WinHttpConnect(session.value, host, parts.nPort, 0));
  if (!connection) {
    return fail(last_error("WinHttpConnect"));
  }
  winhttp::handle request(WinHttpOpenRequest(
      connection.value, L"GET", path, nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
  if (!request) {
    return fail(last_error("WinHttpOpenRequest"));
  }
  DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects,
                   sizeof(redirects));
  if (!WinHttpSetOption(request.value, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
                        nullptr, 0)) {
    return fail(last_error("WinHttpSetOption"));
  }
  if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
    return fail(last_error("WinHttpSendRequest"));
  }
  if (!WinHttpReceiveResponse(request.value, nullptr)) {
    return fail(last_error("WinHttpReceiveResponse"));
  }
  DWORD status = 0;
  DWORD size = sizeof(status);
  WinHttpQueryHeaders(
      request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
  if (status != 101) {
    return fail("the server did not switch to a WebSocket (HTTP " +
                std::to_string(status) + ")");
  }
  HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request.value, 0);
  if (!socket) {
    return fail(last_error("WinHttpWebSocketCompleteUpgrade"));
  }
  // Receives wait for the next message as long as it takes; the caller's
  // pings keep the socket alive.
  DWORD forever = 0;
  WinHttpSetOption(socket, WINHTTP_OPTION_RECEIVE_TIMEOUT, &forever,
                   sizeof(forever));

  std::scoped_lock lock(state_->handles);
  state_->session = std::exchange(session.value, nullptr);
  state_->connection = std::exchange(connection.value, nullptr);
  state_->socket = socket;
  state_->closed = false;
  return true;
}

bool websocket::send(std::string_view text) {
  std::scoped_lock sending(state_->sending);
  HINTERNET socket = state_->current_socket();
  if (!socket) {
    return false;
  }
  const DWORD result = WinHttpWebSocketSend(
      socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
      const_cast<char *>(text.data()), static_cast<DWORD>(text.size()));
  return result == NO_ERROR;
}

std::optional<std::string> websocket::receive(int *close_code) {
  std::scoped_lock receiving(state_->receiving);
  if (close_code) {
    *close_code = 0;
  }
  std::string message;
  char buffer[4096];
  for (;;) {
    HINTERNET socket = state_->current_socket();
    if (!socket) {
      return std::nullopt;
    }
    DWORD read = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
    const DWORD result =
        WinHttpWebSocketReceive(socket, buffer, sizeof(buffer), &read, &type);
    if (result != NO_ERROR) {
      state_->close_all();
      return std::nullopt;
    }
    if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
      USHORT code = 0;
      DWORD reason_size = 0;
      char reason[128];
      WinHttpWebSocketQueryCloseStatus(socket, &code, reason, sizeof(reason),
                                       &reason_size);
      if (close_code) {
        *close_code = code;
      }
      state_->close_all();
      return std::nullopt;
    }
    message.append(buffer, read);
    // The backend's messages are at most a few KiB; a runaway one ends it.
    if (message.size() > (1u << 20)) {
      state_->close_all();
      return std::nullopt;
    }
    if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
      return message;
    }
    if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
      message.clear(); // the backend sends none: skipped
    }
    // *_FRAGMENT_BUFFER_TYPE: more of the same message follows
  }
}

void websocket::close() {
  if (HINTERNET socket = state_->current_socket()) {
    // A polite close first; the handles go either way.
    WinHttpWebSocketShutdown(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                             nullptr, 0);
  }
  state_->close_all();
}

std::string websocket_url(std::string_view base_url, std::string_view path) {
  std::string base(base_url);
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  if (base.rfind("https://", 0) == 0) {
    base = "wss://" + base.substr(8);
  } else if (base.rfind("http://", 0) == 0) {
    base = "ws://" + base.substr(7);
  }
  return base + std::string(path);
}
} // namespace xerces_core
