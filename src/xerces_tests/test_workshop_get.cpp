#include "harness.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>

#include <string>
#include <vector>

#include <xerces_core/launcher_home.hpp>
#include <xerces_core/workshop_get.hpp>

using namespace xerces_core::workshop_get;

TEST_CASE("workshop get: a progress line round trips") {
  const progress line{2804432292ull, "downloading", 1024, 4096, ""};
  const std::string text = to_json(line);
  CHECK_EQ(text, std::string(R"({"id":"2804432292","state":"downloading",)"
                             R"("done":1024,"total":4096})"));
  const auto back = parse_progress(text);
  REQUIRE(back.has_value());
  CHECK(*back == line);

  const progress failed{3171632315ull, "failed", 0, 0, "Steam stopped."};
  CHECK(parse_progress(to_json(failed)) == failed);
  CHECK(!parse_progress("not json").has_value());
  CHECK(!parse_progress(R"({"id":"1"})").has_value());
  // Steam's own lines on stdout, if any, are skipped
  CHECK(
      !parse_progress("Setting breakpad minidump AppID = 311210").has_value());
}

TEST_CASE("workshop get: Steam's item state flags") {
  // EItemState: 1 subscribed, 4 installed, 8 needs update, 16 downloading,
  // 32 download pending
  CHECK(phase_of(0, false) == item_phase::none);
  CHECK(phase_of(0, true) == item_phase::subscribing);
  CHECK(phase_of(1, true) == item_phase::queued);
  CHECK(phase_of(1 | 32, true) == item_phase::queued);
  CHECK(phase_of(1 | 8, true) == item_phase::queued);
  CHECK(phase_of(1 | 16, true) == item_phase::downloading);
  CHECK(phase_of(1 | 4 | 8 | 16, true) == item_phase::downloading);
  CHECK(phase_of(1 | 4, false) == item_phase::installed);
  // Installed but out of date is not done
  CHECK(phase_of(1 | 4 | 8, true) == item_phase::queued);
  CHECK_EQ(std::string(to_string(item_phase::installed)),
           std::string("installed"));
}

TEST_CASE("workshop get: ids") {
  using ids = std::vector<std::uint64_t>;
  CHECK(parse_ids("2804432292") == ids{2804432292ull});
  const ids three{1, 2, 3};
  CHECK(parse_ids("1,2 3,,2") == three);
  CHECK(!parse_ids("").has_value());
  CHECK(!parse_ids("0").has_value());
  CHECK(!parse_ids("12a").has_value());
  CHECK(!parse_ids("-5").has_value());
  CHECK(!parse_ids("1;quit").has_value());
}

TEST_CASE(
    "workshop get: the helper starts with two handles and is waited for") {
  // where.exe stands in for xerces_cli.exe: it takes the same command line,
  // refuses it and ends with a code of its own. What is tested is the start
  // (the handle list, the job, the suspended start resumed) and the wait.
  const std::string root = xerces_core::environment_utf8(L"SystemRoot");
  REQUIRE(!root.empty());
  const std::filesystem::path where =
      xerces_core::path_from_utf8(root) / "System32" / "where.exe";
  REQUIRE(std::filesystem::exists(where));
  std::atomic<void *> process{nullptr};
  std::string error;
  int lines = 0;
  const int code = xerces_core::workshop_get::run_helper(
      where, std::filesystem::temp_directory_path() / "xerces_no_game",
      {311210}, [&](const xerces_core::workshop_get::progress &) { ++lines; },
      &process, &error);
  CHECK(code > 0);
  CHECK(error.empty());
  CHECK(lines == 0);
  CHECK(process.load() == nullptr);
}

TEST_CASE("workshop get: the helper ends with the program that started it") {
  namespace fs = std::filesystem;
  // A stand-in that goes on for as long as it is let: a batch file (Windows
  // starts cmd.exe for it) that adds a line to a file about once a second.
  // It stops by itself when stop.txt appears, so a failed test leaves nothing
  // running.
  const fs::path dir = fs::temp_directory_path() / "xerces_helper_job";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  const fs::path alive = dir / "alive.txt";
  const fs::path stop = dir / "stop.txt";
  {
    std::ofstream script(dir / "helper.cmd", std::ios::binary);
    script << "@echo off\r\n:again\r\nif exist \"%~dp0stop.txt\" exit /b 0\r\n"
              "echo x>>\"%~dp0alive.txt\"\r\nping -n 2 127.0.0.1 >nul\r\n"
              "goto again\r\n";
  }
  // This program again, as the one that starts the helper and waits for it
  wchar_t self[MAX_PATH]{};
  REQUIRE(GetModuleFileNameW(nullptr, self, MAX_PATH) > 0);
  std::wstring command = L"\"" + std::wstring(self) + L"\" --hold-helper \"" +
                         (dir / "helper.cmd").wstring() + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION starter{};
  REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                         &starter));
  CloseHandle(starter.hThread);
  const auto size = [&alive] {
    std::error_code size_ec;
    const auto bytes = fs::file_size(alive, size_ec);
    return size_ec ? std::uintmax_t{0} : bytes;
  };
  // the helper runs: the file appears and grows
  for (int i = 0; i < 100 && size() == 0; ++i) {
    Sleep(100);
  }
  const std::uintmax_t running = size();
  CHECK(running > 0);
  // the starter is ended, as a game that crashes or is closed mid-download
  TerminateProcess(starter.hProcess, 1);
  WaitForSingleObject(starter.hProcess, 5000);
  CloseHandle(starter.hProcess);
  Sleep(1500);
  const std::uintmax_t after = size();
  Sleep(3000);
  const std::uintmax_t later = size();
  CHECK(later == after); // nothing writes any more: the helper went with it
  std::ofstream(stop) << "stop";
  Sleep(1200);
  fs::remove_all(dir, ec);
}
