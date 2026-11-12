#include "ipc.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <sddl.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <random>

#include "json.hpp"
#include "launcher_home.hpp"
#include "log.hpp"

// wingdi.h's GetObject macro would rename rapidjson's GetObject().
#undef GetObject

namespace xerces_core::ipc {
namespace {
std::string utf8(const std::wstring &text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  std::string out(std::size_t(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(),
                      size, nullptr, nullptr);
  return out;
}

std::wstring wide(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int size =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring out(std::size_t(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(),
                      size);
  return out;
}

std::string lower(std::string text) {
  std::ranges::transform(text, text.begin(), [](unsigned char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
  });
  return text;
}

// The executable of a process, as Windows reports it; empty if the process is
// gone. Servers record their own image with the same call (start()), so the
// two strings agree even when the exe was started through a junction, a
// symbolic link or a subst drive, where GetModuleFileNameW keeps the path the
// process was started with.
std::string image_of(HANDLE process) {
  std::wstring buffer(32768, L'\0');
  DWORD size = DWORD(buffer.size());
  if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &size)) {
    return {};
  }
  buffer.resize(size);
  return utf8(buffer);
}

std::string process_image(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) {
    return {};
  }
  std::string image = image_of(process);
  DWORD exit_code = 0;
  const bool running =
      GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
  CloseHandle(process);
  return running ? image : std::string();
}

// A file's identity: the volume's serial number and the file's index on it,
// the same however the path reaches it (junction, link, subst or mapped
// drive, short name, hard link).
struct file_identity {
  DWORD volume = 0;
  DWORD index_high = 0;
  DWORD index_low = 0;
  bool operator==(const file_identity &) const = default;
};

std::optional<file_identity> identity_of(std::string_view path) {
  if (path.empty()) {
    return std::nullopt;
  }
  // Attributes only, sharing everything: a running exe opens fine this way.
  HANDLE file =
      CreateFileW(wide(path).c_str(), FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }
  BY_HANDLE_FILE_INFORMATION info{};
  const BOOL ok = GetFileInformationByHandle(file, &info);
  CloseHandle(file);
  if (!ok) {
    return std::nullopt;
  }
  return file_identity{info.dwVolumeSerialNumber, info.nFileIndexHigh,
                       info.nFileIndexLow};
}

// An access list that lets only the current user in; inherited by files
// created in a folder made with it.
struct user_only_security {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  SECURITY_ATTRIBUTES attributes{};
  bool ok = false;

  explicit user_only_security(bool inherit) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
      return;
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::uint8_t> buffer(size);
    LPWSTR sid = nullptr;
    if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size) &&
        ConvertSidToStringSidW(
            reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &sid)) {
      const std::wstring sddl = std::wstring(L"D:P(A;") +
                                (inherit ? L"OICI" : L"") + L";GA;;;" + sid +
                                L")";
      ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(
               sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) != FALSE;
      LocalFree(sid);
    }
    CloseHandle(token);
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
  }
  ~user_only_security() {
    if (descriptor) {
      LocalFree(descriptor);
    }
  }
};

std::string random_hex(std::size_t bytes) {
  std::random_device device; // rand_s on Windows
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < bytes; ++i) {
    const unsigned value = device() & 0xFF;
    out.push_back(digits[value >> 4]);
    out.push_back(digits[value & 15]);
  }
  return out;
}

// Waits for an overlapped operation, the stop event or the timeout.
// Returns the bytes moved, or nullopt (and cancels it) when it did not end.
std::optional<DWORD> finish(HANDLE handle, OVERLAPPED &overlapped,
                            HANDLE stop_event, DWORD timeout_ms,
                            bool *more_data = nullptr) {
  HANDLE events[2] = {overlapped.hEvent, stop_event};
  const DWORD count = stop_event ? 2 : 1;
  const DWORD wait = WaitForMultipleObjects(count, events, FALSE, timeout_ms);
  if (wait != WAIT_OBJECT_0) {
    CancelIoEx(handle, &overlapped);
    DWORD ignored = 0;
    GetOverlappedResult(handle, &overlapped, &ignored, TRUE);
    return std::nullopt;
  }
  DWORD moved = 0;
  if (!GetOverlappedResult(handle, &overlapped, &moved, FALSE)) {
    if (GetLastError() == ERROR_MORE_DATA && more_data) {
      *more_data = true;
      return moved;
    }
    return std::nullopt;
  }
  return moved;
}

// One whole message from a message-mode pipe.
std::optional<std::string> read_message(HANDLE pipe, HANDLE stop_event,
                                        DWORD timeout_ms, bool *too_big) {
  std::string message;
  std::vector<char> buffer(64 * 1024);
  OVERLAPPED overlapped{};
  overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::optional<std::string> out;
  for (;;) {
    ResetEvent(overlapped.hEvent);
    DWORD got = 0;
    bool more = false;
    if (ReadFile(pipe, buffer.data(), DWORD(buffer.size()), &got, &overlapped)) {
      message.append(buffer.data(), got);
    } else if (GetLastError() == ERROR_MORE_DATA) {
      GetOverlappedResult(pipe, &overlapped, &got, FALSE);
      message.append(buffer.data(), got);
      more = true;
    } else if (GetLastError() == ERROR_IO_PENDING) {
      const auto moved = finish(pipe, overlapped, stop_event, timeout_ms, &more);
      if (!moved) {
        break;
      }
      message.append(buffer.data(), *moved);
    } else {
      break;
    }
    if (message.size() > max_message) {
      if (too_big) {
        *too_big = true;
      }
      break;
    }
    if (!more) {
      out = std::move(message);
      break;
    }
  }
  CloseHandle(overlapped.hEvent);
  return out;
}

bool write_message(HANDLE pipe, std::string_view message, HANDLE stop_event,
                   DWORD timeout_ms) {
  OVERLAPPED overlapped{};
  overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  DWORD written = 0;
  bool ok = false;
  if (WriteFile(pipe, message.data(), DWORD(message.size()), &written,
                &overlapped)) {
    ok = written == message.size();
  } else if (GetLastError() == ERROR_IO_PENDING) {
    const auto moved = finish(pipe, overlapped, stop_event, timeout_ms);
    ok = moved && *moved == message.size();
  }
  CloseHandle(overlapped.hEvent);
  return ok;
}

// Waits until the client closes its end after the reply, so that
// DisconnectNamedPipe does not throw away a reply it has not read yet. A read
// on the server end ends with ERROR_BROKEN_PIPE when the client closes.
// Bounded by timeout_ms and the stop event: FlushFileBuffers did this job
// before, but it waits for the client with no limit, even on an overlapped
// handle, so a client that never read held the only pipe instance and made
// stop() (and the game's exit) hang.
void wait_for_client_close(HANDLE pipe, HANDLE stop_event, DWORD timeout_ms) {
  OVERLAPPED overlapped{};
  overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  char ignored = 0;
  DWORD got = 0;
  if (!ReadFile(pipe, &ignored, 1, &got, &overlapped) &&
      GetLastError() == ERROR_IO_PENDING) {
    finish(pipe, overlapped, stop_event, timeout_ms);
  }
  // Anything else: the client has gone (ERROR_BROKEN_PIPE) or sent another
  // message, which one connection does not take; either way it is done.
  CloseHandle(overlapped.hEvent);
}

std::string id_or_null(const std::string &id) { return id.empty() ? "null" : id; }
} // namespace

bool same_image(std::string_view a, std::string_view b) {
  if (a.empty() || b.empty()) {
    return false;
  }
  const auto first = identity_of(a);
  const auto second = identity_of(b);
  if (first && second) {
    return *first == *second;
  }
  // One of them cannot be opened (a file deleted while its process runs, or
  // no access): only the paths can be compared.
  return lower(std::string(a)) == lower(std::string(b));
}

// ------------------------------------------------------------------ run files
std::string run_entry_to_json(const run_entry &value) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "role", value.role, alloc);
  json::set(doc, "pid", static_cast<std::int64_t>(value.pid), alloc);
  json::set(doc, "pipe", value.pipe, alloc);
  json::set(doc, "image", value.image, alloc);
  json::set(doc, "api", static_cast<std::int64_t>(value.api), alloc);
  json::set(doc, "started", value.started, alloc);
  return json::canonical(doc);
}

std::optional<run_entry> parse_run_entry(std::string_view text) {
  const auto parsed = json::parse(text);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return std::nullopt;
  }
  const auto &root = parsed.document;
  run_entry out;
  out.role = json::get_string(root, "role").value_or("");
  out.pipe = json::get_string(root, "pipe").value_or("");
  out.image = json::get_string(root, "image").value_or("");
  out.started = json::get_string(root, "started").value_or("");
  const auto *pid = json::member(root, "pid");
  const auto *api = json::member(root, "api");
  if (!pid || !pid->IsUint() || out.role.empty() ||
      !out.pipe.starts_with("\\\\.\\pipe\\xerces-")) {
    return std::nullopt;
  }
  out.pid = pid->GetUint();
  out.api = api && api->IsInt() ? api->GetInt() : 0;
  return out;
}

namespace {
// A run file that does not parse is left alone until it is this old.
constexpr std::chrono::seconds unreadable_run_file_age{5};

bool unreadable_for_a_while(const std::filesystem::path &file) {
  std::error_code ec;
  const auto written = std::filesystem::last_write_time(file, ec);
  return !ec && std::filesystem::file_time_type::clock::now() - written >
                    unreadable_run_file_age;
}
} // namespace

std::vector<run_entry> live_run_entries(const std::filesystem::path &run_dir) {
  std::vector<run_entry> out;
  std::error_code ec;
  for (const auto &file : std::filesystem::directory_iterator(run_dir, ec)) {
    if (file.path().extension() != ".json") {
      continue;
    }
    std::ifstream in(file.path(), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    in.close();
    const auto entry = parse_run_entry(text);
    if (!entry) {
      // Not a run file, or not yet: a game from before the server wrote it
      // through a rename empties the file and then writes it, and a file
      // being scanned may not open. Removing it in that moment would make a
      // running game look crashed to the launcher, so it goes only once it
      // has stayed unreadable for a while.
      if (unreadable_for_a_while(file.path())) {
        std::filesystem::remove(file.path(), ec);
      }
      continue;
    }
    const std::string image = process_image(entry->pid);
    // Compared as files, not strings: the same exe reached by another path
    // (a junction, a subst drive) is still the server, and removing its run
    // file would make a running game look crashed to the launcher.
    if (image.empty() || !same_image(image, entry->image)) {
      std::filesystem::remove(file.path(), ec); // gone, or the pid was reused
      continue;
    }
    out.push_back(*entry);
  }
  std::ranges::sort(out, [](const run_entry &a, const run_entry &b) {
    return a.started > b.started; // newest first
  });
  return out;
}

// ----------------------------------------------------------------- JSON-RPC
parsed_request parse_request(std::string_view message) {
  parsed_request out;
  const auto parsed = json::parse(message);
  if (!parsed.ok) {
    out.error_response = make_error("", parse_error, "not JSON: " + parsed.error);
    return out;
  }
  const auto &root = parsed.document;
  std::string id;
  if (root.IsObject()) {
    if (const auto *value = json::member(root, "id");
        value && (value->IsString() || value->IsNumber())) {
      id = json::canonical(*value);
    }
  }
  const auto version = root.IsObject() ? json::get_string(root, "jsonrpc")
                                       : std::optional<std::string>{};
  const auto method = root.IsObject() ? json::get_string(root, "method")
                                      : std::optional<std::string>{};
  if (!root.IsObject() || version != "2.0" || !method || method->empty()) {
    out.error_response =
        make_error(id, invalid_request, "a JSON-RPC 2.0 request object is needed");
    return out;
  }
  request value;
  value.method = *method;
  value.id = id;
  value.params = "{}";
  if (const auto *params = json::member(root, "params")) {
    if (!params->IsObject() && !params->IsArray()) {
      out.error_response =
          make_error(id, invalid_params, "params must be an object or an array");
      return out;
    }
    value.params = json::canonical(*params);
  }
  out.value = std::move(value);
  return out;
}

std::string make_request(std::string_view method, std::string_view params_json,
                         int id) {
  return "{\"id\":" + std::to_string(id) + ",\"jsonrpc\":\"2.0\",\"method\":" +
         json::quote(method) + ",\"params\":" +
         std::string(params_json.empty() ? "{}" : params_json) + "}";
}

std::string make_result(const std::string &id, std::string_view result_json) {
  return "{\"id\":" + id_or_null(id) + ",\"jsonrpc\":\"2.0\",\"result\":" +
         std::string(result_json.empty() ? "null" : result_json) + "}";
}

std::string make_error(const std::string &id, int code,
                       std::string_view message) {
  return "{\"error\":{\"code\":" + std::to_string(code) +
         ",\"message\":" + json::quote(message) + "},\"id\":" + id_or_null(id) +
         ",\"jsonrpc\":\"2.0\"}";
}

std::optional<response> parse_response(std::string_view message) {
  const auto parsed = json::parse(message);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return std::nullopt;
  }
  const auto &root = parsed.document;
  response out;
  if (const auto *result = json::member(root, "result")) {
    out.ok = true;
    out.result = json::canonical(*result);
    return out;
  }
  const auto *error = json::member(root, "error");
  if (!error || !error->IsObject()) {
    return std::nullopt;
  }
  if (const auto *code = json::member(*error, "code"); code && code->IsInt()) {
    out.code = code->GetInt();
  }
  out.message = json::get_string(*error, "message").value_or("");
  return out;
}

// ------------------------------------------------------------------- server
pipe_server::~pipe_server() { stop(); }

bool pipe_server::start(const std::filesystem::path &run_dir, std::string role,
                        handler on_request, std::string *error) {
  const auto fail = [&](std::string why) {
    if (error) {
      *error = std::move(why);
    }
    return false;
  };
  if (thread_.joinable()) {
    return fail("already serving");
  }
  user_only_security folder(true);
  user_only_security pipe_security(false);
  if (!folder.ok || !pipe_security.ok) {
    return fail("could not build a current-user access list");
  }
  std::error_code ec;
  if (!std::filesystem::exists(run_dir, ec) &&
      !CreateDirectoryW(run_dir.wstring().c_str(), &folder.attributes)) {
    return fail("could not create " + utf8(run_dir.wstring()));
  }

  const DWORD pid = GetCurrentProcessId();
  pipe_ = "\\\\.\\pipe\\xerces-" + role + "-" + std::to_string(pid) + "-" +
          random_hex(16);
  HANDLE pipe = CreateNamedPipeW(
      wide(pipe_).c_str(),
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
          PIPE_REJECT_REMOTE_CLIENTS,
      1, 64 * 1024, 64 * 1024, 0, &pipe_security.attributes);
  if (pipe == INVALID_HANDLE_VALUE) {
    return fail("CreateNamedPipe failed (" + std::to_string(GetLastError()) + ")");
  }
  first_instance_ = pipe;
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);

  // The same call clients make (process_image), so the strings match.
  std::string image = image_of(GetCurrentProcess());
  if (image.empty()) {
    std::wstring module(32768, L'\0');
    module.resize(
        GetModuleFileNameW(nullptr, module.data(), DWORD(module.size())));
    image = utf8(module);
  }
  run_entry entry;
  entry.role = role;
  entry.pid = pid;
  entry.pipe = pipe_;
  entry.image = image;
  entry.started = json_log::now_utc();
  run_file_ = run_dir / (role + "-" + std::to_string(pid) + ".json");
  // Through a temporary file and a rename (write_text_file): written in
  // place, the file is empty for a moment, and a launcher or xerces_cli that
  // listed the run folder just then took it for a dead process's.
  if (!write_text_file(run_file_, run_entry_to_json(entry))) {
    CloseHandle(pipe);
    first_instance_ = nullptr;
    return fail("could not write " + utf8(run_file_.wstring()));
  }
  handler_ = std::move(on_request);
  stopping_ = false;
  thread_ = std::thread([this] { serve(); });
  return true;
}

void pipe_server::serve() {
  HANDLE pipe = static_cast<HANDLE>(first_instance_);
  HANDLE stop_event = static_cast<HANDLE>(stop_event_);
  OVERLAPPED overlapped{};
  overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  while (!stopping_) {
    ResetEvent(overlapped.hEvent);
    bool connected = ConnectNamedPipe(pipe, &overlapped) != FALSE;
    if (!connected) {
      const DWORD why = GetLastError();
      if (why == ERROR_PIPE_CONNECTED) {
        connected = true;
      } else if (why == ERROR_IO_PENDING) {
        connected = finish(pipe, overlapped, stop_event, INFINITE).has_value();
      }
    }
    if (stopping_) {
      break;
    }
    // Every step with the client is bounded, so one that stalls (or never
    // reads) holds the only pipe instance for a few seconds at most.
    const DWORD timeout = client_timeout_ms_;
    if (connected) {
      bool too_big = false;
      const auto message = read_message(pipe, stop_event, timeout, &too_big);
      std::string reply;
      if (too_big) {
        reply = make_error("", invalid_request, "the request is too large");
      } else if (message) {
        const parsed_request parsed = parse_request(*message);
        if (!parsed.value) {
          reply = parsed.error_response;
        } else {
          try {
            reply = handler_(*parsed.value);
          } catch (const std::exception &e) {
            reply = make_error(parsed.value->id, internal_error, e.what());
          }
        }
      }
      if (!reply.empty()) {
        ++served_; // before the reply, so a caller that got it sees the count
        if (write_message(pipe, reply, stop_event, timeout)) {
          wait_for_client_close(pipe, stop_event, timeout);
        }
      }
    }
    DisconnectNamedPipe(pipe);
  }
  CloseHandle(overlapped.hEvent);
}

void pipe_server::stop() {
  if (!thread_.joinable()) {
    return;
  }
  stopping_ = true;
  SetEvent(static_cast<HANDLE>(stop_event_));
  // Every wait in serve() also watches the stop event; cancelling whatever
  // I/O is pending on the pipe as well means no call there can keep join()
  // waiting, whatever the client does.
  CancelIoEx(static_cast<HANDLE>(first_instance_), nullptr);
  thread_.join();
  CloseHandle(static_cast<HANDLE>(first_instance_));
  CloseHandle(static_cast<HANDLE>(stop_event_));
  first_instance_ = stop_event_ = nullptr;
  std::error_code ec;
  std::filesystem::remove(run_file_, ec);
}

// ------------------------------------------------------------------- client
call_result call(const run_entry &server, std::string_view method,
                 std::string_view params_json, int timeout_ms) {
  call_result out;
  const std::wstring name = wide(server.pipe);
  HANDLE pipe = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 2; ++attempt) {
    // Identification only: the server cannot act as this user.
    pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                       OPEN_EXISTING,
                       FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
                           SECURITY_IDENTIFICATION,
                       nullptr);
    if (pipe != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) {
      break;
    }
    WaitNamedPipeW(name.c_str(), DWORD(timeout_ms));
  }
  if (pipe == INVALID_HANDLE_VALUE) {
    out.error = "could not connect to " + server.pipe + " (" +
                std::to_string(GetLastError()) + ")";
    return out;
  }
  ULONG server_pid = 0;
  const std::string image = process_image(server.pid);
  if (!GetNamedPipeServerProcessId(pipe, &server_pid) ||
      server_pid != server.pid || image.empty() ||
      !same_image(image, server.image)) {
    CloseHandle(pipe);
    out.error = "the pipe's server is not the process its run file names";
    return out;
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
  const std::string message = make_request(method, params_json, 1);
  if (!write_message(pipe, message, nullptr, DWORD(timeout_ms))) {
    CloseHandle(pipe);
    out.error = "could not send the request";
    return out;
  }
  bool too_big = false;
  const auto reply = read_message(pipe, nullptr, DWORD(timeout_ms), &too_big);
  CloseHandle(pipe);
  if (!reply) {
    out.error = too_big ? "the response is too large"
                        : "no response within " + std::to_string(timeout_ms) +
                              " ms";
    return out;
  }
  out.ok = true;
  out.response = *reply;
  return out;
}
} // namespace xerces_core::ipc
