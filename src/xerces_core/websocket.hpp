#pragma once

// A WebSocket client for the account socket (account.hpp, the backend's
// /v1/ws): WinHTTP's own WebSocket support, so no library ships for it. It
// reaches the same hosts as the backend's requests (winhttp_util.hpp):
// wss:// to the Xerces backend, or ws:// and wss:// to this PC for tests.
// Text messages only, as the backend sends.
//
// One thread receives (receive() blocks); another may send, and close()
// from any thread ends a receive that is waiting.

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace xerces_core {
class websocket {
public:
  websocket();
  ~websocket();
  websocket(const websocket &) = delete;
  websocket &operator=(const websocket &) = delete;

  // url: wss://host[:port]/path or ws://. Blocks until the upgrade is done
  // or failed; error says why.
  bool connect(std::string_view url, std::string *error = nullptr,
               int timeout_ms = 15000);
  bool send(std::string_view text);
  // The next whole text message; nullopt once the socket is closed (by the
  // server, by close(), or by an error), with the close code when the server
  // sent one (else 0).
  std::optional<std::string> receive(int *close_code = nullptr);
  // Closes the socket; a receive waiting in another thread returns nullopt.
  void close();
  bool open() const;

private:
  struct state;
  std::unique_ptr<state> state_;
};

// The socket URL for a backend base URL: https://x -> wss://x/v1/ws,
// http://127.0.0.1:8080 -> ws://127.0.0.1:8080/v1/ws.
std::string websocket_url(std::string_view base_url, std::string_view path);
} // namespace xerces_core
