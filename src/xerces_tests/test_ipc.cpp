// The command interface (docs/DESIGN.md 15.2): JSON-RPC 2.0 messages, run
// files, and a real named pipe served and called inside the test process.

#include "harness.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#include <xerces_core/ipc.hpp>
#include <xerces_core/json.hpp>

using namespace xerces_core;
namespace fs = std::filesystem;

TEST_CASE("ipc: JSON-RPC requests and their errors") {
  const auto ok = ipc::parse_request(
      R"({"jsonrpc":"2.0","method":"status","params":{"a":1},"id":7})");
  REQUIRE(ok.value.has_value());
  CHECK_EQ(ok.value->method, std::string("status"));
  CHECK_EQ(ok.value->params, std::string(R"({"a":1})"));
  CHECK_EQ(ok.value->id, std::string("7"));

  const auto no_params =
      ipc::parse_request(R"({"jsonrpc":"2.0","method":"mods","id":"x"})");
  REQUIRE(no_params.value.has_value());
  CHECK_EQ(no_params.value->params, std::string("{}"));
  CHECK_EQ(no_params.value->id, std::string("\"x\""));

  const auto broken = ipc::parse_request("{not json");
  CHECK(!broken.value.has_value());
  const auto error = ipc::parse_response(broken.error_response);
  REQUIRE(error.has_value());
  CHECK(!error->ok);
  CHECK_EQ(error->code, ipc::parse_error);

  const auto old = ipc::parse_request(R"({"jsonrpc":"1.0","method":"x","id":1})");
  CHECK_EQ(ipc::parse_response(old.error_response)->code, ipc::invalid_request);
  const auto bad_params =
      ipc::parse_request(R"({"jsonrpc":"2.0","method":"x","params":3,"id":1})");
  CHECK_EQ(ipc::parse_response(bad_params.error_response)->code,
           ipc::invalid_params);

  const auto result = ipc::parse_response(ipc::make_result("1", R"({"b":2})"));
  REQUIRE(result.has_value());
  CHECK(result->ok);
  CHECK_EQ(result->result, std::string(R"({"b":2})"));
  CHECK(json::parse(ipc::make_request("load", R"({"extras":["1"]})", 3)).ok);
}

TEST_CASE("ipc: run files round trip and only name xerces pipes") {
  ipc::run_entry entry;
  entry.role = "game";
  entry.pid = 1234;
  entry.pipe = "\\\\.\\pipe\\xerces-game-1234-abcd";
  entry.image = "C:\\Games\\boiii.exe";
  entry.started = "2026-11-05T08:00:00.000Z";
  const auto back = ipc::parse_run_entry(ipc::run_entry_to_json(entry));
  REQUIRE(back.has_value());
  CHECK_EQ(back->pipe, entry.pipe);
  CHECK_EQ(back->pid, entry.pid);
  CHECK_EQ(back->image, entry.image);
  entry.pipe = "\\\\.\\pipe\\someone-else";
  CHECK(!ipc::parse_run_entry(ipc::run_entry_to_json(entry)).has_value());
}

TEST_CASE("ipc: a pipe served and called in one process") {
  const fs::path run = fs::temp_directory_path() / "xerces_ipc_run";
  fs::remove_all(run);
  // A run file left by a process that is gone is cleaned away.
  fs::create_directories(run);
  {
    ipc::run_entry stale;
    stale.role = "game";
    stale.pid = 0xFFFFFFF0u;
    stale.pipe = "\\\\.\\pipe\\xerces-game-0-dead";
    stale.image = "C:\\nowhere.exe";
    std::ofstream(run / "game-dead.json") << ipc::run_entry_to_json(stale);
  }

  ipc::pipe_server server;
  std::string error;
  REQUIRE(server.start(run, "test", [](const ipc::request &request) {
    if (request.method == "echo") {
      return ipc::make_result(request.id, request.params);
    }
    return ipc::make_error(request.id, ipc::method_not_found, "no such method");
  }, &error));

  const auto entries = ipc::live_run_entries(run);
  REQUIRE_EQ(entries.size(), std::size_t{1});
  CHECK_EQ(entries[0].role, std::string("test"));
  CHECK(!fs::exists(run / "game-dead.json"));

  const auto echoed = ipc::call(entries[0], "echo", R"({"x":[1,2]})");
  REQUIRE(echoed.ok);
  const auto response = ipc::parse_response(echoed.response);
  REQUIRE(response.has_value());
  CHECK(response->ok);
  CHECK_EQ(response->result, std::string(R"({"x":[1,2]})"));

  const auto missing = ipc::call(entries[0], "nope", "{}");
  REQUIRE(missing.ok);
  CHECK_EQ(ipc::parse_response(missing.response)->code, ipc::method_not_found);

  // A second call on the same instance works (the server reconnects).
  CHECK(ipc::call(entries[0], "echo", "{}").ok);
  CHECK_EQ(server.served(), std::size_t{3});

  // A run file naming another image is not trusted.
  ipc::run_entry forged = entries[0];
  forged.image = "C:\\Windows\\notepad.exe";
  CHECK(!ipc::call(forged, "echo", "{}").ok);

  server.stop();
  CHECK(ipc::live_run_entries(run).empty());
  CHECK(!ipc::call(entries[0], "echo", "{}", 500).ok);
  fs::remove_all(run);
}

namespace {
std::wstring wide_path(const std::string &utf8) {
  return fs::path(std::u8string(utf8.begin(), utf8.end())).wstring();
}

std::string this_exe() {
  std::wstring buffer(32768, L'\0');
  buffer.resize(
      GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size())));
  const auto text = fs::path(buffer).u8string();
  return {text.begin(), text.end()};
}

// A client that connects and sends a request, then never reads the reply
// and keeps its handle open (a script stopped in a debugger, say).
HANDLE stuck_client(const std::string &pipe) {
  HANDLE handle =
      CreateFileW(wide_path(pipe).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                  nullptr, OPEN_EXISTING, 0, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return handle;
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  SetNamedPipeHandleState(handle, &mode, nullptr, nullptr);
  const std::string request = ipc::make_request("echo", "{}", 1);
  DWORD written = 0;
  WriteFile(handle, request.data(), DWORD(request.size()), &written, nullptr);
  return handle;
}

bool wait_until(const std::function<bool()> &done, int ms) {
  const auto until =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!done()) {
    if (std::chrono::steady_clock::now() > until) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}
} // namespace

TEST_CASE("ipc: a client that never reads neither blocks others nor stop()") {
  const fs::path run = fs::temp_directory_path() / "xerces_ipc_stuck";
  fs::remove_all(run);
  const auto echo = [](const ipc::request &request) {
    return ipc::make_result(request.id, request.params);
  };

  {
    ipc::pipe_server server;
    server.set_client_timeout(300);
    REQUIRE(server.start(run, "test", echo));
    HANDLE stuck = stuck_client(server.pipe_name());
    REQUIRE(stuck != INVALID_HANDLE_VALUE);
    CHECK(wait_until([&] { return server.served() == 1; }, 2000));
    // The server gives up on the stuck client and takes the next one.
    const auto entries = ipc::live_run_entries(run);
    REQUIRE_EQ(entries.size(), std::size_t{1});
    const auto next = ipc::call(entries[0], "echo", R"({"n":2})", 3000);
    CHECK(next.ok);
    CHECK_EQ(server.served(), std::size_t{2});
    CloseHandle(stuck);
    server.stop();
  }

  {
    // With the default timeout, stop() still returns at once: the game calls
    // it while exiting.
    ipc::pipe_server server;
    REQUIRE(server.start(run, "test", echo));
    HANDLE stuck = stuck_client(server.pipe_name());
    REQUIRE(stuck != INVALID_HANDLE_VALUE);
    CHECK(wait_until([&] { return server.served() == 1; }, 2000));
    // Should stop() wait for the client again, the client lets go after a
    // few seconds, so the check below fails instead of the test run hanging.
    std::atomic<bool> stopped = false;
    std::thread rescue([&] {
      wait_until([&] { return stopped.load(); }, 4000);
      CloseHandle(stuck);
    });
    const auto started = std::chrono::steady_clock::now();
    server.stop();
    const auto took = std::chrono::steady_clock::now() - started;
    stopped = true;
    rescue.join();
    CHECK(took < std::chrono::milliseconds(1500));
  }

  {
    // The same for a client that connects and sends nothing.
    ipc::pipe_server server;
    REQUIRE(server.start(run, "test", echo));
    HANDLE silent = CreateFileW(wide_path(server.pipe_name()).c_str(),
                                GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    REQUIRE(silent != INVALID_HANDLE_VALUE);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto started = std::chrono::steady_clock::now();
    server.stop();
    CHECK(std::chrono::steady_clock::now() - started <
          std::chrono::milliseconds(1500));
    CloseHandle(silent);
  }
  fs::remove_all(run);
}

TEST_CASE("ipc: the server's image is compared as a file, not a string") {
  const fs::path dir = fs::temp_directory_path() / "xerces_ipc_image";
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::ofstream(dir / "a.exe") << "a";
  std::ofstream(dir / "c.exe") << "c";
  const auto utf8 = [](const fs::path &path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
  };
  const std::string a = utf8(dir / "a.exe");
  CHECK(ipc::same_image(a, a));
  CHECK(!ipc::same_image(a, utf8(dir / "c.exe")));
  // Another name for the same file.
  if (CreateHardLinkW((dir / "b.exe").wstring().c_str(),
                      (dir / "a.exe").wstring().c_str(), nullptr)) {
    CHECK(ipc::same_image(a, utf8(dir / "b.exe")));
  }
  // Files that cannot be opened fall back to the path, in any case.
  CHECK(ipc::same_image("C:\\Nowhere\\X.exe", "c:\\nowhere\\x.EXE"));
  CHECK(!ipc::same_image("C:\\Nowhere\\X.exe", "C:\\Nowhere\\Y.exe"));
  CHECK(!ipc::same_image("", ""));
  fs::remove_all(dir);

  // A run file naming this exe by another path (here its volume GUID path,
  // which reaches the file the way a junction or subst drive does) still
  // names the running server: it is kept, and calls go through.
  const std::string exe = this_exe();
  wchar_t volume[MAX_PATH]{};
  const std::wstring root = fs::path(wide_path(exe)).root_path().wstring();
  if (GetVolumeNameForVolumeMountPointW(root.c_str(), volume, MAX_PATH)) {
    const std::string other =
        utf8(fs::path(volume) / fs::path(wide_path(exe)).relative_path());
    CHECK(other != exe);
    CHECK(ipc::same_image(exe, other));

    const fs::path run = fs::temp_directory_path() / "xerces_ipc_other_path";
    fs::remove_all(run);
    ipc::pipe_server server;
    REQUIRE(server.start(run, "test", [](const ipc::request &request) {
      return ipc::make_result(request.id, "1");
    }));
    auto entries = ipc::live_run_entries(run);
    REQUIRE_EQ(entries.size(), std::size_t{1});
    ipc::run_entry moved = entries[0];
    moved.image = other;
    for (const auto &file : fs::directory_iterator(run)) {
      std::ofstream(file.path(), std::ios::binary | std::ios::trunc)
          << ipc::run_entry_to_json(moved);
    }
    entries = ipc::live_run_entries(run);
    REQUIRE_EQ(entries.size(), std::size_t{1});
    CHECK_EQ(entries[0].image, other);
    CHECK(ipc::call(entries[0], "echo", "{}").ok);
    server.stop();
    fs::remove_all(run);
  }
}
