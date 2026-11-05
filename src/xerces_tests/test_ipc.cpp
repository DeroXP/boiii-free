// The command interface (docs/DESIGN.md 15.2): JSON-RPC 2.0 messages, run
// files, and a real named pipe served and called inside the test process.

#include "harness.hpp"

#include <filesystem>
#include <fstream>
#include <string>

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
