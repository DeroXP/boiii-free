#include "crash_outbox.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include "journal.hpp"
#include "json.hpp"
#include "launch.hpp"
#include "launcher_config.hpp"
#include "launcher_home.hpp"
#include "log_tail.hpp"

#undef GetObject

namespace xerces_core {
namespace fs = std::filesystem;

namespace {
std::string read_bytes(const fs::path &path, std::size_t limit) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  std::string out;
  out.resize(limit + 1);
  in.read(out.data(), static_cast<std::streamsize>(out.size()));
  out.resize(static_cast<std::size_t>(in.gcount()));
  return out;
}

std::int64_t unix_time(const fs::file_time_type &time) {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::clock_cast<std::chrono::system_clock>(time)
                 .time_since_epoch())
      .count();
}

std::string lower(std::string text) {
  for (char &c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

fs::path outbox_dir(const fs::path &crash_dir) { return crash_dir / "outbox"; }
fs::path pending_dir(const fs::path &crash_dir) {
  return crash_dir / "pending";
}

void remove_file(const fs::path &file) {
  std::error_code ec;
  if (!file.empty()) {
    fs::remove(file, ec);
  }
}

// The lines of a log file that hold an event, newest last.
std::vector<std::string> lines_with(const std::vector<std::string> &lines,
                                    std::string_view event) {
  const std::string needle = "\"event\":\"" + std::string(event) + "\"";
  std::vector<std::string> out;
  for (const auto &line : lines) {
    if (line.find(needle) != std::string::npos) {
      out.push_back(line);
    }
  }
  return out;
}

// data.<key> of a log line, as a string.
std::optional<std::string> data_string(const std::string &line,
                                       std::string_view key) {
  const auto parsed = json::parse(line);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return std::nullopt;
  }
  const auto *data = json::member(parsed.document, "data");
  if (data == nullptr || !data->IsObject()) {
    return std::nullopt;
  }
  return json::get_string(*data, key);
}

std::vector<std::string> data_lines(const std::string &line,
                                    std::string_view key) {
  std::vector<std::string> out;
  const auto parsed = json::parse(line);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return out;
  }
  const auto *data = json::member(parsed.document, "data");
  const auto *list =
      data != nullptr && data->IsObject() ? json::member(*data, key) : nullptr;
  if (list != nullptr && list->IsArray()) {
    for (const auto &item : list->GetArray()) {
      if (item.IsString()) {
        out.emplace_back(item.GetString(), item.GetStringLength());
      }
    }
  }
  return out;
}

// "2026-11-05T09:48:15.123Z" as a log name's "20261105-094815".
std::string journal_start_key(std::string_view started) {
  std::string out;
  for (const char c : started.substr(0, 19)) {
    if (c >= '0' && c <= '9') {
      out.push_back(c);
    }
  }
  if (out.size() != 14) {
    return {};
  }
  return out.substr(0, 8) + "-" + out.substr(8);
}

struct home_facts {
  std::string journal_text;
  std::optional<launch_journal> journal;
  plan_mods plan;
  std::optional<std::string> plan_id;
  launcher_config config;
};

home_facts read_home(const fs::path &home, std::string_view journal_override,
                     const std::string &config_text) {
  home_facts out;
  out.journal_text =
      journal_override.empty()
          ? read_text_file(home / "logs" / "journal.json").value_or("")
          : std::string(journal_override);
  out.journal = parse_journal(out.journal_text);
  if (out.journal && !out.journal->plan_id.empty()) {
    out.plan_id = out.journal->plan_id;
    const std::string id = plan_file_id(out.journal->plan_id);
    // Only a hex id names a plan file (as the bundle checks).
    if (!id.empty() && std::ranges::all_of(id, [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) {
      if (const auto text =
              read_text_file(home / "config" / "plans" / (id + ".json"))) {
        out.plan = read_plan_mods(*text);
      }
    }
  }
  if (!config_text.empty()) {
    out.config = parse_launcher_config(config_text).value;
  }
  return out;
}

std::vector<std::string> log_lines(const std::optional<fs::path> &file,
                                   std::size_t n) {
  if (!file) {
    return {};
  }
  const auto tail = tail_file(*file, n);
  return tail ? tail->lines : std::vector<std::string>{};
}

// The flags a launch from this config passed, for a report the client did
// not write: -nointro, the extra flags, and -xerces-db-pool when the journal
// says so.
std::vector<std::string> config_flags(const home_facts &facts) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  const auto add = [&](std::string flag) {
    if (flag.empty()) {
      return;
    }
    if (flag[0] != '-') {
      flag = "-" + flag;
    }
    if (seen.insert(flag).second) {
      out.push_back(flag);
    }
  };
  if (facts.config.nointro) {
    add("-nointro");
  }
  for (const auto &flag : facts.config.extra_flags) {
    add(flag);
  }
  if (facts.journal && facts.journal->db_pool) {
    add("-xerces-db-pool");
  }
  return launch_flag_names(out);
}

crash_report_input base_input(const crash_context &ctx, const home_facts &facts,
                              std::string report_id) {
  crash_report_input in;
  in.report_id = std::move(report_id);
  in.created_utc = ctx.now_utc;
  in.version = ctx.version;
  in.commit = ctx.commit;
  in.dirty = ctx.dirty;
  in.game_checksum = ctx.game_checksum;
  in.steam_build = ctx.steam_build;
  if (ctx.system) {
    in.system = ctx.system();
  }
  in.mode = facts.journal && !facts.journal->mode.empty() ? facts.journal->mode
                                                          : facts.config.mode;
  in.plan_id = facts.plan_id;
  in.mods = facts.plan.mods;
  in.safe_mode = facts.journal && facts.journal->safe_mode;
  in.journal_json = facts.journal ? facts.journal_text : std::string();
  in.launcher_log = log_lines(newest_log(ctx.home / "logs", "launcher"),
                              crash_launcher_log_lines);
  in.profile = ctx.profile;
  in.user_name = ctx.user_name;
  const crash_redaction words = redaction_from_config(ctx.config_text);
  in.player_names = words.player_names;
  in.known_addresses = words.known_addresses;
  return in;
}

// The map the crash happened on: the last map that finished loading when
// the game was in a match, the map being loaded when it was loading one.
std::optional<std::string> crash_map(const std::vector<std::string> &game_log,
                                     std::string_view phase,
                                     const home_facts &facts) {
  if (phase == "in_game") {
    const auto ready = lines_with(game_log, "loader.map.ready");
    if (!ready.empty()) {
      if (auto map = data_string(ready.back(), "map"); map && !map->empty()) {
        return map;
      }
    }
  }
  if (phase == "loading_map") {
    const auto oom = lines_with(game_log, "loader.out_of_memory");
    if (!oom.empty()) {
      if (auto map = data_string(oom.back(), "map"); map && !map->empty()) {
        return map;
      }
    }
    return facts.plan.map;
  }
  return std::nullopt;
}

bool plain_log_name(std::string_view name) {
  return !name.empty() && name.size() < 200 &&
         name.find_first_of("\\/:") == std::string_view::npos &&
         name.ends_with(".jsonl") && name.find("..") == std::string_view::npos;
}

void write_marker(const fs::path &crash_dir, const home_facts &facts) {
  if (facts.journal && !facts.journal->started.empty()) {
    write_text_file(crash_dir / "reported.txt", facts.journal->started);
  }
}
} // namespace

const char *to_string(send_outcome outcome) {
  switch (outcome) {
  case send_outcome::sent:
    return "sent";
  case send_outcome::duplicate:
    return "duplicate";
  case send_outcome::rejected:
    return "rejected";
  case send_outcome::retry:
    return "retry";
  case send_outcome::off:
    return "off";
  case send_outcome::not_taken:
    return "not_taken";
  }
  return "retry";
}

std::optional<std::string>
crash_base_url_from(std::optional<std::string> value) {
  if (!value || value->empty()) {
    return std::string(crash_default_base);
  }
  std::string url = *value;
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  // Only a backend on this PC: every host Xerces talks to is listed in
  // docs/ENDPOINTS.md, so an environment variable cannot send the reports
  // anywhere else.
  const std::string low = lower(url);
  std::string rest;
  if (low.starts_with("https://")) {
    rest = low.substr(8);
  } else if (low.starts_with("http://")) {
    rest = low.substr(7);
  } else {
    return std::nullopt;
  }
  const std::string authority = rest.substr(0, rest.find_first_of("/?#"));
  if (authority.find('@') != std::string::npos) {
    return std::nullopt; // "https://127.0.0.1@elsewhere"
  }
  std::string host = authority;
  std::string port;
  if (host.starts_with("[")) {
    const std::size_t close = host.find(']');
    if (close == std::string::npos) {
      return std::nullopt;
    }
    port = host.substr(close + 1);
    host = host.substr(0, close + 1);
  } else if (const std::size_t colon = host.find(':');
             colon != std::string::npos) {
    port = host.substr(colon);
    host = host.substr(0, colon);
  }
  if (!port.empty() && (port[0] != ':' || port.size() < 2 ||
                        !std::ranges::all_of(port.substr(1), [](char c) {
                          return c >= '0' && c <= '9';
                        }))) {
    return std::nullopt;
  }
  if (host == "127.0.0.1" || host == "localhost" || host == "[::1]") {
    return url;
  }
  return std::nullopt;
}

std::optional<std::string> crash_base_url() {
  char *value = nullptr;
  std::size_t size = 0;
  std::optional<std::string> set;
  if (_dupenv_s(&value, &size, "XERCES_CRASH_URL") == 0 && value != nullptr) {
    set = std::string(value);
  }
  std::free(value);
  return crash_base_url_from(set);
}

std::string crash_install_id(const fs::path &home, bool create) {
  const fs::path file = home / "config" / "install_id";
  if (const auto text = read_text_file(file)) {
    std::string id;
    for (const char c : *text) {
      if (!std::isspace(static_cast<unsigned char>(c))) {
        id.push_back(c);
      }
    }
    if (valid_report_id(id)) {
      return id;
    }
  }
  if (!create) {
    return {};
  }
  const std::string id = new_report_id();
  if (!valid_report_id(id) || !write_text_file(file, id + "\n")) {
    return {};
  }
  return id;
}

std::vector<outbox_entry> list_outbox(const fs::path &crash_dir) {
  std::vector<outbox_entry> out;
  std::error_code ec;
  for (fs::directory_iterator it(outbox_dir(crash_dir), ec), end;
       !ec && it != end; it.increment(ec)) {
    const fs::path &file = it->path();
    if (file.extension() != ".json" ||
        !valid_report_id(path_utf8(file.stem()))) {
      continue;
    }
    std::error_code item;
    outbox_entry entry;
    entry.report_id = path_utf8(file.stem());
    entry.file = file;
    entry.size = it->file_size(item);
    const auto time = it->last_write_time(item);
    entry.queued = item ? 0 : unix_time(time);
    out.push_back(std::move(entry));
  }
  std::ranges::sort(out, [](const outbox_entry &a, const outbox_entry &b) {
    return a.queued != b.queued ? a.queued < b.queued
                                : a.report_id < b.report_id;
  });
  return out;
}

std::size_t prune_outbox(const fs::path &crash_dir, std::int64_t now,
                         std::size_t max_count, std::int64_t max_age) {
  std::vector<outbox_entry> entries = list_outbox(crash_dir);
  std::size_t removed = 0;
  std::vector<outbox_entry> kept;
  for (auto &entry : entries) {
    if (now - entry.queued > max_age) {
      remove_file(entry.file);
      ++removed;
    } else {
      kept.push_back(std::move(entry));
    }
  }
  // Oldest first: the oldest go when there are too many.
  while (kept.size() > max_count) {
    remove_file(kept.front().file);
    kept.erase(kept.begin());
    ++removed;
  }
  return removed;
}

bool enqueue_report(const fs::path &crash_dir, std::string_view report_id,
                    std::string_view body, std::int64_t now) {
  if (!valid_report_id(report_id)) {
    return false;
  }
  const bool ok = write_text_file(
      outbox_dir(crash_dir) / (std::string(report_id) + ".json"), body);
  prune_outbox(crash_dir, now);
  return ok;
}

std::size_t clear_outbox(const fs::path &crash_dir) {
  std::size_t removed = 0;
  for (const auto &entry : list_outbox(crash_dir)) {
    remove_file(entry.file);
    ++removed;
  }
  return removed;
}

std::vector<pending_entry> list_pending(const fs::path &crash_dir) {
  struct found {
    fs::path json, dump;
    std::int64_t time = 0;
  };
  std::map<std::string, found> by_id;
  std::error_code ec;
  for (fs::directory_iterator it(pending_dir(crash_dir), ec), end;
       !ec && it != end; it.increment(ec)) {
    const fs::path &file = it->path();
    const std::string id = path_utf8(file.stem());
    if (!valid_report_id(id)) {
      continue;
    }
    std::error_code item;
    const auto time = it->last_write_time(item);
    found &entry = by_id[id];
    entry.time = std::max(entry.time, item ? 0 : unix_time(time));
    if (file.extension() == ".json") {
      entry.json = file;
    } else if (file.extension() == ".dmp") {
      entry.dump = file;
    }
  }
  std::vector<std::pair<std::int64_t, pending_entry>> list;
  for (auto &[id, entry] : by_id) {
    pending_entry pending;
    pending.json_file = entry.json;
    pending.dump_file = entry.dump;
    std::optional<pending_crash> crash;
    if (!entry.json.empty()) {
      crash = parse_pending_crash(read_text_file(entry.json).value_or(""));
    }
    if (crash && crash->report_id == id) {
      pending.crash = std::move(*crash);
    } else {
      // A dump whose note was never written (the process ended first), or
      // a note that cannot be read: still a crash of the client's.
      pending.crash.report_id = id;
    }
    list.emplace_back(entry.time, std::move(pending));
  }
  std::ranges::sort(list, [](const auto &a, const auto &b) {
    return a.first != b.first
               ? a.first < b.first
               : a.second.crash.report_id < b.second.crash.report_id;
  });
  std::vector<pending_entry> out;
  for (auto &[time, entry] : list) {
    out.push_back(std::move(entry));
  }
  return out;
}

std::string report_from_pending(const pending_entry &pending,
                                std::optional<std::uint32_t> exit_code,
                                const crash_context &ctx) {
  const home_facts facts = read_home(
      ctx.home,
      pending.crash.journal.empty() ? ctx.journal_text : pending.crash.journal,
      ctx.config_text);
  crash_report_input in = base_input(ctx, facts, pending.crash.report_id);
  if (!pending.crash.created_utc.empty()) {
    in.created_utc = pending.crash.created_utc;
  }
  in.crash = crash_from_pending(pending.crash, exit_code);
  if (pending.crash.code == 0 && exit_code) {
    in.crash.code = crash_code_hex(*exit_code);
  }
  if (in.crash.phase.empty() && facts.journal) {
    in.crash.phase = to_string(facts.journal->phase);
  }
  in.flags = pending.crash.flags.empty()
                 ? config_flags(facts)
                 : launch_flag_names(pending.crash.flags);
  std::optional<fs::path> game_log;
  if (plain_log_name(pending.crash.game_log)) {
    game_log = ctx.home / "logs" / path_from_utf8(pending.crash.game_log);
  }
  in.game_log = log_lines(game_log, crash_game_log_lines);
  in.map = crash_map(in.game_log, in.crash.phase, facts);
  in.console = pending.crash.console;
  in.details = pending.crash.details;
  // The names the game knew: its own (which launcher.json often does not
  // hold) and the players it was in a match with.
  for (const auto &name : pending.crash.names) {
    if (std::ranges::find(in.player_names, name) == in.player_names.end()) {
      in.player_names.push_back(name);
    }
  }
  if (!pending.dump_file.empty()) {
    const std::string dump = read_bytes(pending.dump_file, crash_minidump_cap);
    std::vector<std::string> words = in.player_names;
    words.push_back(in.user_name);
    // Masked before it is queued; a file that is not a minidump is left out
    // (the server would refuse the whole report).
    if (dump.size() <= crash_minidump_cap) {
      in.minidump = mask_minidump(dump, words).value_or("");
    }
  }
  return build_crash_report(in);
}

std::string report_from_exit(std::string_view report_id,
                             std::uint32_t exit_code,
                             const crash_context &ctx) {
  const home_facts facts =
      read_home(ctx.home, ctx.journal_text, ctx.config_text);
  crash_report_input in = base_input(ctx, facts, std::string(report_id));
  // The game's own log of this launch: the newest one, when it started after
  // the launcher wrote the journal (a game that crashed before its log
  // opened has none).
  const auto newest = newest_log(ctx.home / "logs", "game");
  const std::string start =
      facts.journal ? journal_start_key(facts.journal->started) : std::string();
  if (newest && (start.empty() || log_start(*newest) >= start)) {
    in.game_log = log_lines(newest, crash_game_log_lines);
  }
  const bool oom = !lines_with(in.game_log, "loader.out_of_memory").empty();
  in.crash = crash_from_exit(exit_code, oom,
                             facts.journal ? to_string(facts.journal->phase)
                                           : std::string());
  in.flags = config_flags(facts);
  in.map = crash_map(in.game_log, in.crash.phase, facts);
  // The console's last lines, from the game's own record of how it ended.
  for (const char *event : {"loader.out_of_memory", "loader.exit.unexpected"}) {
    const auto lines = lines_with(in.game_log, event);
    if (!lines.empty()) {
      in.console = data_lines(lines.back(), "console");
      if (!in.console.empty()) {
        break;
      }
    }
  }
  return build_crash_report(in);
}

collect_result collect_crashes(const crash_context &ctx, bool send_enabled,
                               std::optional<std::uint32_t> exit_code) {
  collect_result out;
  const fs::path crash_dir = ctx.home / "crash";
  std::vector<pending_entry> pending = list_pending(crash_dir);
  const home_facts facts =
      read_home(ctx.home, ctx.journal_text, ctx.config_text);
  const bool journal_clean =
      facts.journal && facts.journal->phase == launch_phase::clean_exit;
  out.crashed =
      exit_code && exit_is_crash(*exit_code, journal_clean, !pending.empty());

  // Old pending crashes go as old reports would.
  std::erase_if(pending, [&](const pending_entry &entry) {
    std::error_code ec;
    const fs::path file =
        entry.json_file.empty() ? entry.dump_file : entry.json_file;
    const auto time = fs::last_write_time(file, ec);
    if (!ec && ctx.now - unix_time(time) > outbox_max_age_seconds) {
      remove_file(entry.json_file);
      remove_file(entry.dump_file);
      return true;
    }
    return false;
  });

  // Half-written files of a handler that was ended mid-write (<id>.dmp.tmp,
  // <id>.json.tmp): list_pending skips them, and nothing removed them
  // (review, 2026-11-13). An hour old: the game writes them in seconds.
  {
    std::vector<fs::path> stale;
    std::error_code ec;
    for (fs::directory_iterator it(pending_dir(crash_dir), ec), end;
         !ec && it != end; it.increment(ec)) {
      std::error_code item;
      const auto time = it->last_write_time(item);
      if (it->path().extension() == ".tmp" && !item &&
          ctx.now - unix_time(time) > 3600) {
        stale.push_back(it->path());
      }
    }
    for (const fs::path &file : stale) {
      remove_file(file);
    }
  }

  if (!send_enabled) {
    // Off: nothing waits to be sent, so turning it on later never sends a
    // crash from this time.
    for (const auto &entry : pending) {
      remove_file(entry.json_file);
      remove_file(entry.dump_file);
      ++out.dropped;
    }
    out.dropped += clear_outbox(crash_dir);
    write_marker(crash_dir, facts);
    return out;
  }

  for (std::size_t i = 0; i < pending.size(); ++i) {
    const pending_entry &entry = pending[i];
    // The newest is the game that just exited; the exit code is its.
    const bool newest = i + 1 == pending.size();
    const std::string body =
        report_from_pending(entry, newest ? exit_code : std::nullopt, ctx);
    if (enqueue_report(crash_dir, entry.crash.report_id, body, ctx.now)) {
      out.queued.push_back(entry.crash.report_id);
      remove_file(entry.json_file);
      remove_file(entry.dump_file);
    }
  }
  if (pending.empty() && out.crashed) {
    // A crash the client wrote nothing for: once per launch, so a second
    // `xerces_cli crash send --exit-code` does not report it again.
    const std::string reported =
        read_text_file(crash_dir / "reported.txt").value_or("");
    const bool already = facts.journal && !facts.journal->started.empty() &&
                         reported == facts.journal->started;
    if (!already) {
      const std::string id = new_report_id();
      if (valid_report_id(id) &&
          enqueue_report(crash_dir, id, report_from_exit(id, *exit_code, ctx),
                         ctx.now)) {
        out.queued.push_back(id);
      }
    }
  }
  if (!out.queued.empty() || out.crashed) {
    write_marker(crash_dir, facts);
  }
  return out;
}

namespace {
std::vector<std::pair<std::string, std::string>>
headers_for(const send_options &options, bool gzip) {
  std::vector<std::pair<std::string, std::string>> headers{
      {"Content-Type", "application/json"},
      {"X-Xerces-Install", options.install_id},
      {"User-Agent", options.user_agent},
  };
  if (gzip) {
    headers.emplace_back("Content-Encoding", "gzip");
  }
  return headers;
}

http_response post_report(const send_options &options,
                          const crash_transport &transport,
                          const std::string &body) {
  http_request request;
  request.method = "POST";
  request.url = options.base_url + "/v1/crashes";
  std::string compressed = options.gzip ? gzip_bytes(body) : std::string();
  const bool gzip = !compressed.empty();
  request.headers = headers_for(options, gzip);
  request.body = gzip ? std::move(compressed) : body;
  return transport(request);
}

// The body without its minidump, for a second try after a 413.
std::optional<std::string> without_minidump(const std::string &body) {
  auto parsed = json::parse(body);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return std::nullopt;
  }
  auto &doc = parsed.document;
  const auto *dump = json::member(doc, "minidump_b64");
  if (dump == nullptr || dump->IsNull()) {
    return std::nullopt;
  }
  json::set(doc, "minidump_b64", rapidjson::Value(), doc.GetAllocator());
  return json::canonical(doc);
}
} // namespace

service_answer ask_service(const send_options &options,
                           const crash_transport &transport,
                           http_response *response) {
  http_request request;
  request.method = "GET";
  request.url = options.base_url + "/v1/crashes";
  // No install id: asking says nothing about this install.
  request.headers = {{"User-Agent", options.user_agent}};
  http_response answer = transport(request);
  const service_answer out =
      !answer.reached        ? service_answer::no_answer
      : answer.status == 404 ? service_answer::not_yet
      : (answer.status >= 200 && answer.status < 300) || answer.status == 405
          ? service_answer::takes_reports
          : service_answer::no_answer;
  if (response) {
    *response = std::move(answer);
  }
  return out;
}

std::vector<send_result> send_outbox(const fs::path &crash_dir,
                                     const send_options &options,
                                     const crash_transport &transport) {
  std::vector<send_result> out;
  // The outbox's limits hold when sending too: a report older than 7 days
  // is removed, not sent, even when no crash queued one since.
  prune_outbox(crash_dir,
               options.now != 0
                   ? options.now
                   : std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count());
  bool asked = false;
  // With the install id once it is known.
  send_options sending = options;
  for (const outbox_entry &entry : list_outbox(crash_dir)) {
    send_result result;
    result.report_id = entry.report_id;
    const bool no_id = sending.install_id.empty() && !options.make_install_id;
    if ((options.enabled && !options.enabled()) || no_id ||
        options.base_url.empty()) {
      result.outcome = send_outcome::off;
      result.error = no_id                      ? "no install id"
                     : options.base_url.empty() ? "no address to send to"
                                                : "sending is off";
      out.push_back(std::move(result));
      break;
    }
    if (!asked) {
      // Once a run, before any report leaves: a server without crash
      // routes would only turn every report away.
      asked = true;
      http_response answer;
      const service_answer service = ask_service(options, transport, &answer);
      if (service != service_answer::takes_reports) {
        result.status = answer.status;
        result.retry_after = answer.retry_after;
        if (service == service_answer::not_yet) {
          result.outcome = send_outcome::not_taken;
          result.error = "the server does not take crash reports yet";
        } else {
          result.outcome = send_outcome::retry;
          result.error = answer.reached
                             ? "HTTP " + std::to_string(answer.status)
                             : answer.error;
        }
        out.push_back(std::move(result));
        break;
      }
      if (sending.install_id.empty()) {
        sending.install_id = options.make_install_id();
        if (sending.install_id.empty()) {
          result.outcome = send_outcome::off;
          result.error = "no install id";
          out.push_back(std::move(result));
          break;
        }
      }
    }
    std::string body = read_text_file(entry.file).value_or("");
    if (body.empty()) {
      remove_file(entry.file);
      continue;
    }
    http_response response = post_report(sending, transport, body);
    if (response.reached && response.status == 413) {
      if (const auto smaller = without_minidump(body)) {
        response = post_report(sending, transport, *smaller);
      }
    }
    result.status = response.status;
    result.retry_after = response.retry_after;
    result.error = response.error;
    if (!response.reached) {
      result.outcome = send_outcome::retry;
    } else if (response.status == 201 || response.status == 200) {
      const auto parsed = json::parse(response.body);
      if (parsed.ok && parsed.document.IsObject()) {
        result.server_id = json::get_string(parsed.document, "id").value_or("");
      }
      result.outcome =
          response.status == 201 ? send_outcome::sent : send_outcome::duplicate;
      remove_file(entry.file);
    } else if (response.status == 400 || response.status == 413 ||
               response.status == 422) {
      result.outcome = send_outcome::rejected;
      result.error =
          "the server refused it (" + std::to_string(response.status) + ")";
      remove_file(entry.file);
    } else {
      result.outcome = send_outcome::retry;
      result.error = "HTTP " + std::to_string(response.status);
    }
    const bool stop = result.outcome == send_outcome::retry;
    out.push_back(std::move(result));
    if (stop) {
      break;
    }
  }
  return out;
}

bool reports_wait(const std::vector<send_result> &results) {
  return results.empty() ||
         std::ranges::any_of(results, [](const send_result &result) {
           return result.outcome == send_outcome::retry ||
                  result.outcome == send_outcome::off ||
                  result.outcome == send_outcome::not_taken;
         });
}

std::optional<std::int64_t>
delete_remote_reports(const send_options &options,
                      const crash_transport &transport, std::string *error) {
  if (options.install_id.empty() || options.base_url.empty()) {
    if (error) {
      *error = options.install_id.empty() ? "no install id"
                                          : "no address to send to";
    }
    return std::nullopt;
  }
  http_request request;
  request.method = "DELETE";
  request.url = options.base_url + "/v1/crashes";
  request.headers = {{"X-Xerces-Install", options.install_id},
                     {"User-Agent", options.user_agent}};
  const http_response response = transport(request);
  if (!response.reached || response.status != 200) {
    if (error) {
      *error = response.reached ? "HTTP " + std::to_string(response.status)
                                : response.error;
    }
    return std::nullopt;
  }
  const auto parsed = json::parse(response.body);
  if (parsed.ok && parsed.document.IsObject()) {
    if (const auto *count = json::member(parsed.document, "deleted");
        count != nullptr && count->IsInt64()) {
      return count->GetInt64();
    }
  }
  return 0;
}
} // namespace xerces_core
