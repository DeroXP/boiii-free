// Crash reports (docs/CRASH_REPORTS.md): the contract's field names (a golden
// report), redaction, the outbox's limits, the setting, the send rules with a
// fake transport, gzip, and how a crash and a crashing module are told apart.

#include "harness.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <zlib.h>

#include <xerces_core/crash_module.hpp>
#include <xerces_core/crash_outbox.hpp>
#include <xerces_core/crash_report.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/launcher_config.hpp>

using namespace xerces_core;
namespace fs = std::filesystem;

namespace {
void write(const fs::path &path, const std::string &text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string slurp(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string gunzip(const std::string &bytes) {
  z_stream stream{};
  if (inflateInit2(&stream, 15 + 16) != Z_OK) {
    return {};
  }
  std::string out(1 << 20, '\0');
  stream.next_in = reinterpret_cast<const Bytef *>(bytes.data());
  stream.avail_in = static_cast<uInt>(bytes.size());
  stream.next_out = reinterpret_cast<Bytef *>(out.data());
  stream.avail_out = static_cast<uInt>(out.size());
  const int result = inflate(&stream, Z_FINISH);
  out.resize(stream.total_out);
  inflateEnd(&stream);
  return result == Z_STREAM_END ? out : std::string();
}

// A transport's answer; error set means it never reached a server.
http_response reply(int status, std::string body,
                    std::optional<int> retry_after = std::nullopt) {
  http_response out;
  out.reached = true;
  out.status = status;
  out.body = std::move(body);
  out.retry_after = retry_after;
  return out;
}

std::int64_t now_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// A fixed report input: every field of the contract set.
crash_report_input sample_input() {
  crash_report_input in;
  in.report_id = std::string(32, 'a');
  in.created_utc = "2026-11-05T12:00:00.000Z";
  in.version = "0.1.0-beta.1";
  in.commit = "3f09512d";
  in.dirty = false;
  in.game_checksum = 0x6531394;
  in.steam_build = 24784313;
  in.system.windows = "10.0.26300";
  in.system.cpu = "Intel(R) Core(TM) Ultra 9 285K";
  in.system.logical_cpus = 24;
  in.system.ram_mib = 65536;
  in.system.gpus = {{"NVIDIA GeForce RTX 5070 Ti", "32.0.15.8180"}};
  in.system.display = "3840x2160@240";
  in.mode = "zm";
  in.map = "zm_castle";
  in.plan_id = "sha256:" + std::string(64, 'b');
  in.mods = {{"ws.1", "DS4C", "1.0.0"}};
  in.flags = {"-nointro", "-xerces-db-pool"};
  in.safe_mode = false;
  pending_crash pending;
  pending.code = 0xC0000005;
  pending.module = "BlackOps3.exe";
  pending.offset = 0x14224FE5Cull;
  pending.thread = "main";
  pending.frames = {"BlackOps3.exe+0x14224FE5C", "boiii.exe+0x1234"};
  pending.phase = "in_game";
  in.crash = crash_from_pending(pending, 0xC0000005);
  in.journal_json = R"({"phase":"in_game","schema":1})";
  in.game_log = {R"({"event":"loader.map.ready","msg":"zm_castle"})"};
  in.launcher_log = {R"({"event":"launcher.start","msg":"Xerces"})"};
  in.console = {"Loading fastfile 'zm_castle'"};
  in.minidump = "MDMP";
  return in;
}
} // namespace

TEST_CASE("crash report: the contract's fields match the golden report") {
  const std::string body = build_crash_report(sample_input());
  const std::string golden = xerces_tests::read_file(
      xerces_tests::fixtures_dir() / "crash" / "report_golden.json");
  REQUIRE(!golden.empty());
  const auto expected = json::parse(golden);
  REQUIRE(expected.ok);
  CHECK_EQ(body, json::canonical(expected.document));

  // Every name the backend reads, with its JSON type.
  const auto parsed = json::parse(body);
  REQUIRE(parsed.ok);
  const auto &doc = parsed.document;
  CHECK_EQ(json::get_string(doc, "schema").value_or(""),
           std::string("xerces.crash/1"));
  for (const char *key : {"report_id", "created_utc"}) {
    CHECK(json::member(doc, key) && json::member(doc, key)->IsString());
  }
  for (const char *key :
       {"xerces", "game", "system", "launch", "crash", "logs"}) {
    CHECK(json::member(doc, key) && json::member(doc, key)->IsObject());
  }
  const auto &crash = *json::member(doc, "crash");
  for (const char *key : {"kind", "code", "exit_code", "module", "offset",
                          "thread", "frames", "phase"}) {
    CHECK(json::member(crash, key) != nullptr);
  }
  const auto &logs = *json::member(doc, "logs");
  for (const char *key :
       {"journal", "game_log_tail", "launcher_log_tail", "console_tail"}) {
    CHECK(json::member(logs, key) != nullptr);
  }
  CHECK(json::member(doc, "minidump_b64")->IsString());
  CHECK_EQ(json::get_string(doc, "minidump_b64").value_or(""),
           std::string("TURNUA=="));
}

TEST_CASE("crash report: missing parts are null, caps hold") {
  crash_report_input in = sample_input();
  in.map.reset();
  in.plan_id.reset();
  in.game_checksum.reset();
  in.steam_build.reset();
  in.minidump.clear();
  in.journal_json.clear();
  in.crash = crash_from_exit(0xC0000409, false, "frontend");
  in.mods = {{std::string(1000, 'x'), "name", "1"}};
  in.console = std::vector<std::string>(5000, std::string(100, 'c'));
  in.crash.frames = std::vector<std::string>(50, "m+0x1");
  const auto parsed = json::parse(build_crash_report(in));
  REQUIRE(parsed.ok);
  const auto &doc = parsed.document;
  CHECK(json::member(doc, "minidump_b64")->IsNull());
  CHECK(json::member(*json::member(doc, "game"), "build")->IsNull());
  CHECK(json::member(*json::member(doc, "game"), "steam_build")->IsNull());
  const auto &launch = *json::member(doc, "launch");
  CHECK(json::member(launch, "map")->IsNull());
  CHECK(json::member(launch, "plan_id")->IsNull());
  CHECK_EQ(json::get_string(json::member(launch, "mods")->GetArray()[0], "id")
               ->size(),
           crash_name_cap);
  const auto &crash = *json::member(doc, "crash");
  CHECK_EQ(json::get_string(crash, "kind").value_or(""),
           std::string("fail_fast"));
  CHECK(json::member(crash, "module")->IsNull());
  CHECK_EQ(json::member(crash, "frames")->Size(), rapidjson::SizeType(32));
  CHECK(json::member(*json::member(doc, "logs"), "journal")->IsNull());
  const std::string console =
      json::get_string(*json::member(doc, "logs"), "console_tail").value_or("");
  CHECK(console.size() <= crash_tail_cap);
  CHECK(!console.empty());
  // Too large a dump is left out rather than cut.
  in.minidump = std::string(crash_minidump_cap + 1, 'd');
  const auto big = json::parse(build_crash_report(in));
  CHECK(json::member(big.document, "minidump_b64")->IsNull());
}

TEST_CASE("crash report: no user name, path, player or address survives") {
  crash_report_input in = sample_input();
  in.profile = "C:\\Users\\Ginld";
  in.user_name = "Ginld";
  in.player_names = {"DeroXP"};
  in.known_addresses = {"Dave's house", "dave.example.org"};
  in.game_log = {
      R"({"msg":"C:\\Users\\Ginld\\XercesHome\\logs","data":{"home":"c:/users/ginld/x"}})",
      R"({"msg":"joined 203.0.113.7:27017 as DeroXP","data":{"server":"Dave's house"}})",
      R"({"msg":"D:\\Users\\SomeoneElse\\BO3\\BlackOps3.exe"})",
      R"({"msg":"steam 76561198012345678 xuid 0x11000010abcdef1 id 0110000100abcdef"})",
  };
  in.launcher_log = {
      R"({"msg":"dave.example.org without a port; user Ginld"})"};
  in.console = {"Connecting to 198.51.100.20:27017...",
                "C:/Users/Ginld/Desktop/thing.txt", "Player DeroXP joined"};
  const std::string body = build_crash_report(in);
  for (const char *secret :
       {"Ginld", "ginld", "SomeoneElse", "DeroXP", "203.0.113.7",
        "198.51.100.20", "Dave's house", "dave.example.org",
        "76561198012345678", "11000010abcdef1", "0110000100abcdef"}) {
    CHECK(body.find(secret) == std::string::npos);
  }
  CHECK(body.find("%USERPROFILE%") != std::string::npos);
  CHECK(body.find("<user>") != std::string::npos);
  CHECK(body.find("<player>") != std::string::npos);
  CHECK(body.find("<steam id>") != std::string::npos);
  CHECK(body.find("<xuid>") != std::string::npos);
  CHECK(json::parse(body).ok); // still JSON after scrubbing

  const crash_redaction words{"", "", {}, {}};
  CHECK_EQ(
      redact_crash_text("E:\\Users\\Bob\\x and E:/Users/Bob/y", words, false),
      std::string("E:\\Users\\<user>\\x and E:/Users/<user>/y"));
  CHECK_EQ(redact_crash_text("C:\\Program Files\\Steam", words, false),
           std::string("C:\\Program Files\\Steam"));
}

TEST_CASE("crash report: the tail keeps the end, cut at a line") {
  CHECK_EQ(tail_text("short", 100), std::string("short"));
  CHECK_EQ(tail_text("first line\nsecond\nthird", 14),
           std::string("second\nthird"));
  CHECK_EQ(tail_text("aaaa\nbbbb\n", 6), std::string("bbbb\n"));
}

TEST_CASE("crash report: a crash is told apart from a normal exit") {
  CHECK(!exit_is_crash(0, true, false));
  CHECK(!exit_is_crash(0, false, false)); // the dialog's restart, a quick quit
  CHECK(!exit_is_crash(1, true, false));  // console close after quit
  CHECK(exit_is_crash(0xC0000005, false, false));
  CHECK(exit_is_crash(0, true, true)); // a dump is always a crash
  CHECK(classify_exit(0xC0000409, false) == crash_kind::fail_fast);
  CHECK(classify_exit(0xFFFFFFFF, true) == crash_kind::out_of_memory);
  CHECK(classify_exit(0xFFFFFFFF, false) == crash_kind::unknown_exit);
  CHECK(classify_exit(1, false) == crash_kind::unknown_exit);
  const crash_facts exit = crash_from_exit(0xC0000409, false, "launching");
  CHECK_EQ(exit.code, std::string("0xC0000409"));
  CHECK_EQ(exit.exit_code.value_or(""), std::string("0xC0000409"));
  CHECK_EQ(crash_frame("BlackOps3.exe", 0x14224FE5C),
           std::string("BlackOps3.exe+0x14224FE5C"));
  CHECK_EQ(crash_frame("", 0x10), std::string("unknown+0x10"));
}

TEST_CASE("crash module: the client is the client, wherever its folder") {
  const std::string client = "C:\\Users\\Ginld\\Xerces\\boiii.exe";
  const std::string game =
      "D:\\SteamLibrary\\steamapps\\common\\Call of Duty Black Ops III";
  const std::string windows = "C:\\Windows";
  // The misdiagnosis: boiii.exe outside the game folder was "other software".
  CHECK(classify_module(client, client, game, windows) ==
        module_origin::client);
  CHECK(classify_module("c:/users/ginld/xerces/BOIII.EXE", client, game,
                        windows) == module_origin::client);
  CHECK(classify_module("\\\\?\\C:\\Users\\Ginld\\Xerces\\boiii.exe", client,
                        game, windows) == module_origin::client);
  // Another boiii.exe is not this client.
  CHECK(classify_module("C:\\Other\\boiii.exe", client, game, windows) ==
        module_origin::other_software);
  CHECK(classify_module(game + "\\BlackOps3.exe", client, game, windows) ==
        module_origin::game);
  CHECK(classify_module(game + "\\dxgi.dll", client, game, windows) ==
        module_origin::game_proxy);
  CHECK(classify_module(
            "C:\\Program Files (x86)\\Steam\\GameOverlayRenderer64.dll", client,
            game, windows) == module_origin::overlay);
  CHECK(classify_module(
            "C:\\Windows\\System32\\DriverStore\\FileRepository\\nv\\nvwgf2umx."
            "dll",
            client, game, windows) == module_origin::graphics_driver);
  CHECK(classify_module("C:\\Windows\\System32\\d3d11.dll", client, game,
                        windows) == module_origin::directx);
  CHECK(classify_module("C:\\Windows\\System32\\ntdll.dll", client, game,
                        windows) == module_origin::windows);
  CHECK(classify_module("C:\\WindowsApps\\x.dll", client, game, windows) ==
        module_origin::other_software);
  CHECK(classify_module("C:\\Program Files\\Antivirus\\hook.dll", client, game,
                        windows) == module_origin::other_software);
  CHECK(classify_module(client, "", "", windows) ==
        module_origin::other_software);
  CHECK_EQ(overlay_name("RTSSHooks64.dll").value_or(""),
           std::string_view("RivaTuner Statistics Server (MSI Afterburner)"));
  CHECK_EQ(module_file_name("C:/A/B/Thing.DLL"), std::string("thing.dll"));
}

TEST_CASE("crash report: the client's pending note round-trips") {
  pending_crash in;
  in.report_id = std::string(32, 'c');
  in.created_utc = "2026-11-05T12:00:00.000Z";
  in.code = 0xC0000005;
  in.module = "boiii.exe";
  in.offset = 0x1A2B;
  in.thread = "main";
  in.frames = {"boiii.exe+0x1A2B"};
  in.phase = "frontend";
  in.console = {"line 1", "line 2"};
  in.flags = {"-nointro"};
  in.game_log = "20261105-120000-game-42.jsonl";
  in.journal = R"({"phase":"frontend","schema":1})";
  const auto out = parse_pending_crash(pending_crash_to_json(in));
  REQUIRE(out.has_value());
  CHECK_EQ(out->report_id, in.report_id);
  CHECK_EQ(out->code, in.code);
  CHECK_EQ(out->module, in.module);
  CHECK(out->offset == in.offset);
  CHECK(out->frames == in.frames);
  CHECK(out->console == in.console);
  CHECK(out->flags == in.flags);
  CHECK_EQ(out->game_log, in.game_log);
  CHECK_EQ(out->journal, in.journal);
  CHECK(!parse_pending_crash(R"({"report_id":"not-hex"})"));
  CHECK(launch_flag_names(
            {"-xerces-plan", "abc", "-nointro", "+set", "a", "-x"}) ==
        (std::vector<std::string>{"-xerces-plan", "-nointro"}));
  CHECK(valid_report_id(new_report_id()));
  CHECK(new_report_id() != new_report_id());
}

TEST_CASE("crash report: the setting is on by default and kept") {
  CHECK(launcher_config{}.send_crash_reports);
  CHECK(crash_reports_enabled("{}"));
  CHECK(!crash_reports_enabled(R"({"send_crash_reports":false})"));
  CHECK(!crash_reports_enabled("not json")); // a broken file sends nothing
  launcher_config config;
  config.send_crash_reports = false;
  const auto back = parse_launcher_config(launcher_config_to_json(config));
  CHECK(!back.value.send_crash_reports);
  const crash_redaction words = redaction_from_config(
      R"({"player_name":"Me","favorites":[{"name":"Home","address":"h.example.org:1"}],"recent_servers":["1.2.3.4:5"]})");
  CHECK(words.player_names == std::vector<std::string>{"Me"});
  CHECK_EQ(words.known_addresses.size(), std::size_t{3});
}

TEST_CASE("crash report: the dialog's note says what happens") {
  const std::string off = crash_report_note(true, false, true);
  CHECK(off.find(std::string(crash_issues_url)) != std::string::npos);
  CHECK(off.find("Diagnostics > Copy report") != std::string::npos);
  CHECK(crash_report_note(false, true, true)
            .find(std::string(crash_issues_url)) != std::string::npos);
  CHECK(
      crash_report_note(true, true, true).find("as soon as the game closes") !=
      std::string::npos);
  CHECK(crash_report_note(true, true, false).find("when you open it next") !=
        std::string::npos);
  CHECK(crash_report_note(true, true, false).find("Ezz") == std::string::npos);
}

TEST_CASE("crash outbox: at most 20 reports, none older than 7 days") {
  const fs::path home = fs::temp_directory_path() / "xerces_crash_outbox";
  fs::remove_all(home);
  const fs::path crash = home / "crash";
  const std::int64_t now = now_seconds();
  for (int i = 0; i < 25; ++i) {
    char id[33];
    std::snprintf(id, sizeof(id), "%032x", i);
    CHECK(enqueue_report(crash, id, "{}", now));
  }
  CHECK_EQ(list_outbox(crash).size(), outbox_max_reports);
  CHECK(!enqueue_report(crash, "../escape", "{}", now));
  // Backdate one past the limit: it goes at the next prune.
  const auto entries = list_outbox(crash);
  fs::last_write_time(entries.back().file, fs::file_time_type::clock::now() -
                                               std::chrono::hours(24 * 8));
  CHECK_EQ(prune_outbox(crash, now), std::size_t{1});
  CHECK_EQ(list_outbox(crash).size(), outbox_max_reports - 1);
  CHECK_EQ(clear_outbox(crash), outbox_max_reports - 1);
  CHECK(list_outbox(crash).empty());
  fs::remove_all(home);
}

TEST_CASE("crash outbox: the base URL and the install id") {
  CHECK_EQ(crash_base_url_from(std::nullopt).value_or(""),
           std::string(crash_default_base));
  CHECK_EQ(
      crash_base_url_from(std::string("https://x.example.org/")).value_or(""),
      std::string("https://x.example.org"));
  CHECK_EQ(
      crash_base_url_from(std::string("http://127.0.0.1:8000")).value_or(""),
      std::string("http://127.0.0.1:8000"));
  CHECK(crash_base_url_from(std::string("http://localhost:8000")).has_value());
  CHECK(crash_base_url_from(std::string("http://[::1]:8000")).has_value());
  CHECK(!crash_base_url_from(std::string("http://evil.example.org")));
  CHECK(!crash_base_url_from(std::string("http://127.0.0.1.example.org")));
  CHECK(!crash_base_url_from(std::string("ftp://x")));

  const fs::path home = fs::temp_directory_path() / "xerces_crash_install";
  fs::remove_all(home);
  const std::string id = crash_install_id(home);
  CHECK(valid_report_id(id));
  CHECK_EQ(crash_install_id(home), id);
  CHECK(fs::exists(home / "config" / "install_id"));
  write(home / "config" / "install_id", "garbage");
  const std::string fresh = crash_install_id(home);
  CHECK(valid_report_id(fresh));
  CHECK(fresh != id);
  fs::remove_all(home);
}

TEST_CASE("crash outbox: gzip bodies inflate back") {
  const std::string body(5000, 'x');
  const std::string packed = gzip_bytes(body);
  REQUIRE(packed.size() > 2);
  CHECK_EQ(static_cast<unsigned char>(packed[0]), 0x1Fu);
  CHECK_EQ(static_cast<unsigned char>(packed[1]), 0x8Bu);
  CHECK(packed.size() < body.size());
  CHECK_EQ(gunzip(packed), body);
}

TEST_CASE("crash outbox: with sending off nothing is sent or queued") {
  const fs::path home = fs::temp_directory_path() / "xerces_crash_off";
  fs::remove_all(home);
  const fs::path crash = home / "crash";
  enqueue_report(crash, std::string(32, 'a'), "{}", now_seconds());
  int calls = 0;
  const crash_transport fake = [&](const http_request &) {
    ++calls;
    return reply(201, R"({"id":"x","duplicate":false})");
  };
  send_options options;
  options.base_url = "https://x.example.org";
  options.install_id = std::string(32, 'f');
  options.user_agent = "Xerces/test";
  options.enabled = [] { return false; };
  const auto results = send_outbox(crash, options, fake);
  CHECK_EQ(calls, 0);
  REQUIRE_EQ(results.size(), std::size_t{1});
  CHECK(results[0].outcome == send_outcome::off);
  CHECK_EQ(list_outbox(crash).size(), std::size_t{1}); // kept, not sent

  // The client's pending crash is dropped, not queued for later.
  pending_crash pending;
  pending.report_id = std::string(32, 'b');
  pending.code = 0xC0000005;
  write(crash / "pending" / (pending.report_id + ".json"),
        pending_crash_to_json(pending));
  write(crash / "pending" / (pending.report_id + ".dmp"), "MDMP");
  crash_context ctx;
  ctx.home = home;
  ctx.now = now_seconds();
  const collect_result collected = collect_crashes(ctx, false, 0xC0000005);
  CHECK(collected.queued.empty());
  CHECK_EQ(collected.dropped, std::size_t{2}); // the pending crash, the queue
  CHECK(collected.crashed);
  CHECK(list_pending(crash).empty());
  CHECK(list_outbox(crash).empty());
  CHECK_EQ(calls, 0);
  fs::remove_all(home);
}

TEST_CASE("crash outbox: sent, duplicate, refused and retried reports") {
  const fs::path home = fs::temp_directory_path() / "xerces_crash_send";
  fs::remove_all(home);
  const fs::path crash = home / "crash";
  const std::int64_t now = now_seconds();
  const auto queue = [&](char c, const std::string &body, int age) {
    const std::string id(32, c);
    enqueue_report(crash, id, body, now);
    fs::last_write_time(crash / "outbox" / (id + ".json"),
                        fs::file_time_type::clock::now() -
                            std::chrono::seconds(100 - age));
  };
  queue('1', R"({"n":1})", 1);
  queue('2', R"({"n":2})", 2);
  queue('3', R"({"minidump_b64":"QUJD","n":3})", 3);
  queue('4', R"({"n":4})", 4);
  queue('5', R"({"n":5})", 5);
  std::vector<http_request> seen;
  const crash_transport fake = [&](const http_request &request) {
    seen.push_back(request);
    const std::string body = gunzip(request.body);
    if (body.find("\"n\":1") != std::string::npos) {
      return reply(201, R"({"id":"srv-1","duplicate":false})");
    }
    if (body.find("\"n\":2") != std::string::npos) {
      return reply(200, R"({"id":"srv-2","duplicate":true})");
    }
    if (body.find("\"n\":3") != std::string::npos) {
      // Too large with the dump; fine without it.
      return body.find("QUJD") != std::string::npos
                 ? reply(413, "{}")
                 : reply(201, R"({"id":"srv-3","duplicate":false})");
    }
    if (body.find("\"n\":4") != std::string::npos) {
      return reply(400, "{}");
    }
    return reply(429, "{}", 60);
  };
  send_options options;
  options.base_url = "https://x.example.org";
  options.install_id = std::string(32, 'f');
  options.user_agent = "Xerces/test";
  options.enabled = [] { return true; };
  const auto results = send_outbox(crash, options, fake);
  REQUIRE_EQ(results.size(), std::size_t{5});
  CHECK(results[0].outcome == send_outcome::sent);
  CHECK_EQ(results[0].server_id, std::string("srv-1"));
  CHECK(results[1].outcome == send_outcome::duplicate);
  CHECK(results[2].outcome == send_outcome::sent);
  CHECK(results[3].outcome == send_outcome::rejected);
  CHECK(results[4].outcome == send_outcome::retry);
  CHECK(results[4].retry_after == std::optional<int>(60));
  REQUIRE_EQ(seen.size(), std::size_t{6}); // the 413 was tried twice
  const auto &first = seen.front();
  CHECK_EQ(first.method, std::string("POST"));
  CHECK_EQ(first.url, std::string("https://x.example.org/v1/crashes"));
  const auto header = [&](const std::string &name) {
    for (const auto &[key, value] : first.headers) {
      if (key == name) {
        return value;
      }
    }
    return std::string();
  };
  CHECK_EQ(header("X-Xerces-Install"), options.install_id);
  CHECK_EQ(header("Content-Encoding"), std::string("gzip"));
  CHECK_EQ(header("Content-Type"), std::string("application/json"));
  CHECK_EQ(header("User-Agent"), std::string("Xerces/test"));
  // Only the rate-limited one waits for the next try.
  const auto left = list_outbox(crash);
  REQUIRE_EQ(left.size(), std::size_t{1});
  CHECK_EQ(left[0].report_id, std::string(32, '5'));

  // No answer at all: kept, and the run stops there.
  const crash_transport down = [](const http_request &) {
    http_response out;
    out.error = "no route";
    return out;
  };
  queue('6', R"({"n":6})", 6);
  const auto offline = send_outbox(crash, options, down);
  REQUIRE_EQ(offline.size(), std::size_t{1});
  CHECK(offline[0].outcome == send_outcome::retry);
  CHECK_EQ(list_outbox(crash).size(), std::size_t{2});

  // DELETE: every report of this install.
  std::string method;
  const crash_transport deleting = [&](const http_request &request) {
    method = request.method;
    return reply(200, R"({"deleted":3})");
  };
  CHECK(delete_remote_reports(options, deleting) ==
        std::optional<std::int64_t>(3));
  CHECK_EQ(method, std::string("DELETE"));
  fs::remove_all(home);
}

TEST_CASE("crash outbox: pending crashes and exits become reports") {
  const fs::path home = fs::temp_directory_path() / "xerces_crash_collect";
  fs::remove_all(home);
  const fs::path crash = home / "crash";
  const std::string plan(64, 'e');
  write(
      home / "logs" / "journal.json",
      R"({"db_pool":true,"launch_id":"","mode":"zm","phase":"loading_map","plan_id":"sha256:)" +
          plan +
          R"(","safe_mode":false,"schema":1,"stack":["ws.1"],"started":"2026-11-05T10:00:00.000Z","updated":""})");
  write(
      home / "config" / "plans" / (plan + ".json"),
      R"({"mode":"zm","stack":[{"id":"ws.1","name":"DS4C","version":"1.2.0"}],"map":{"id":"ws.9","name":"Mario","folder":"zm_mario64","version":"1.0.0"}})");
  write(
      home / "logs" / "20261105-100001-game-7.jsonl",
      R"({"data":{"console":["Loading fastfile 'zm_mario64'"],"map":"zm_mario64"},"event":"loader.out_of_memory","msg":"out of memory"})"
      "\n");
  write(home / "logs" / "20261105-095959-launcher-1.jsonl",
        R"({"event":"launcher.start","msg":"Xerces"})"
        "\n");
  crash_context ctx;
  ctx.home = home;
  ctx.version = "0.1.0-test";
  ctx.commit = "abc";
  ctx.now = now_seconds();
  ctx.now_utc = "2026-11-05T10:05:00.000Z";
  ctx.config_text = R"({"nointro":true,"extra_flags":["-console"]})";
  ctx.system = [] {
    crash_system system;
    system.windows = "10.0.1";
    return system;
  };

  // The out-of-memory box: no dump, exit 0xFFFFFFFF.
  collect_result result = collect_crashes(ctx, true, 0xFFFFFFFFu);
  CHECK(result.crashed);
  REQUIRE_EQ(result.queued.size(), std::size_t{1});
  {
    const auto outbox = list_outbox(crash);
    REQUIRE_EQ(outbox.size(), std::size_t{1});
    const auto report = json::parse(slurp(outbox[0].file));
    REQUIRE(report.ok);
    const auto &c = *json::member(report.document, "crash");
    CHECK_EQ(json::get_string(c, "kind").value_or(""),
             std::string("out_of_memory"));
    CHECK_EQ(json::get_string(c, "phase").value_or(""),
             std::string("loading_map"));
    const auto &launch = *json::member(report.document, "launch");
    CHECK_EQ(json::get_string(launch, "map").value_or(""),
             std::string("zm_mario64"));
    CHECK_EQ(json::member(launch, "mods")->Size(), rapidjson::SizeType(1));
    CHECK_EQ(json::member(launch, "flags")->Size(), rapidjson::SizeType(3));
    const auto &logs = *json::member(report.document, "logs");
    CHECK(json::get_string(logs, "console_tail")->find("zm_mario64") !=
          std::string::npos);
    CHECK(json::get_string(logs, "launcher_log_tail")->find("launcher.start") !=
          std::string::npos);
  }
  // The same launch is not reported twice.
  result = collect_crashes(ctx, true, 0xFFFFFFFFu);
  CHECK(result.queued.empty());
  CHECK_EQ(list_outbox(crash).size(), std::size_t{1});

  // A pending crash from the client: reported with its dump, then gone.
  pending_crash pending;
  pending.report_id = std::string(32, 'd');
  pending.code = 0xC0000005;
  pending.module = "BlackOps3.exe";
  pending.offset = 0x140001000;
  pending.frames = {"BlackOps3.exe+0x140001000"};
  pending.phase = "in_game";
  write(crash / "pending" / (pending.report_id + ".json"),
        pending_crash_to_json(pending));
  write(crash / "pending" / (pending.report_id + ".dmp"), "MDMP");
  // And a dump whose note was never written.
  write(crash / "pending" / (std::string(32, '9') + ".dmp"), "MDMP");
  result = collect_crashes(ctx, true, std::nullopt);
  CHECK_EQ(result.queued.size(), std::size_t{2});
  CHECK(list_pending(crash).empty());
  const std::string body =
      slurp(crash / "outbox" / (pending.report_id + ".json"));
  const auto report = json::parse(body);
  REQUIRE(report.ok);
  CHECK_EQ(json::get_string(*json::member(report.document, "crash"), "kind")
               .value_or(""),
           std::string("exception"));
  CHECK_EQ(json::get_string(report.document, "minidump_b64").value_or(""),
           std::string("TURNUA=="));
  fs::remove_all(home);
}
