#pragma once

// The command interface between Xerces processes (docs/DESIGN.md 3.1, 15.2):
// each running process serves JSON-RPC 2.0 on a named pipe with a random
// per-session name, created with PIPE_REJECT_REMOTE_CLIENTS and
// FILE_FLAG_FIRST_PIPE_INSTANCE and an access list for the current user only,
// and announces it in <home>\run\<role>-<pid>.json. Clients check the server's
// process id and image path before talking. One message is one JSON
// document (message-mode pipe), at most max_message bytes.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace xerces_core::ipc {
inline constexpr int api_version = 1;
inline constexpr std::size_t max_message = 1 << 20;

// JSON-RPC 2.0 error codes.
inline constexpr int parse_error = -32700;
inline constexpr int invalid_request = -32600;
inline constexpr int method_not_found = -32601;
inline constexpr int invalid_params = -32602;
inline constexpr int internal_error = -32603;
inline constexpr int busy = -32000;  // the game cannot do it right now
inline constexpr int refused = -32001; // not allowed in this state

// One <home>\run\ file.
struct run_entry {
  std::string role; // "game", "launcher"
  std::uint32_t pid = 0;
  std::string pipe;  // \\.\pipe\xerces-...
  std::string image; // the process's executable, UTF-8
  int api = api_version;
  std::string started; // UTC
};

std::string run_entry_to_json(const run_entry &value);
std::optional<run_entry> parse_run_entry(std::string_view text);
// Entries in run_dir whose process is still running with the image it named;
// files of processes that are gone are removed.
std::vector<run_entry> live_run_entries(const std::filesystem::path &run_dir);

struct request {
  std::string method;
  std::string params; // canonical JSON (object or array), "{}" if absent
  std::string id;     // canonical JSON (number or string); empty: notification
};

// The request in a message, or the error response to send back instead.
struct parsed_request {
  std::optional<request> value;
  std::string error_response;
};
parsed_request parse_request(std::string_view message);

std::string make_request(std::string_view method, std::string_view params_json,
                         int id);
std::string make_result(const std::string &id, std::string_view result_json);
std::string make_error(const std::string &id, int code,
                       std::string_view message);

// A response's result (canonical JSON) or its error.
struct response {
  bool ok = false;
  std::string result;
  int code = 0;
  std::string message;
};
std::optional<response> parse_response(std::string_view message);

// Called on the server's thread for every request; returns the whole response
// message (make_result or make_error).
using handler = std::function<std::string(const request &)>;

class pipe_server {
public:
  pipe_server() = default;
  pipe_server(const pipe_server &) = delete;
  pipe_server &operator=(const pipe_server &) = delete;
  ~pipe_server();

  // Creates the pipe, writes <run_dir>\<role>-<pid>.json and serves on its
  // own thread, one client at a time.
  bool start(const std::filesystem::path &run_dir, std::string role,
             handler on_request, std::string *error = nullptr);
  // Stops serving and removes the run file. Returns promptly whatever a
  // connected client is doing.
  void stop();
  const std::string &pipe_name() const { return pipe_; }
  std::size_t served() const { return served_; }

  // How long a client gets for each step: sending its request, taking the
  // reply, and closing its end once it has read it. A client that misses a
  // step is disconnected, so it cannot keep others out for longer. Matches
  // call()'s default timeout; set before start() (tests shorten it).
  void set_client_timeout(std::uint32_t ms) { client_timeout_ms_ = ms; }

private:
  void serve();

  std::string pipe_;
  std::filesystem::path run_file_;
  handler handler_;
  void *first_instance_ = nullptr; // the pipe created with FIRST_PIPE_INSTANCE
  void *stop_event_ = nullptr;
  std::thread thread_;
  std::atomic<bool> stopping_ = false;
  std::atomic<std::size_t> served_ = 0;
  std::uint32_t client_timeout_ms_ = 5000;
};

// Whether two executable paths (UTF-8) name the same file: compared by volume
// serial number and file index when both can be opened, so a junction, a
// symbolic link or a subst drive does not make one exe look like another;
// by lower-cased path otherwise.
bool same_image(std::string_view a, std::string_view b);

struct call_result {
  bool ok = false;        // a response arrived (it may still be an error)
  std::string response;   // the raw response message
  std::string error;      // why there is none
};

// Connects, checks the server is the process the entry names, sends one
// request and reads the response.
call_result call(const run_entry &server, std::string_view method,
                 std::string_view params_json, int timeout_ms = 5000);
} // namespace xerces_core::ipc
