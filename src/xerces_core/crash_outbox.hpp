#pragma once

// Sending crash reports (docs/CRASH_REPORTS.md): the outbox in
// <home>\crash\outbox, the install id, turning the client's pending crashes
// into reports, and the POST and DELETE calls to the backend. The transport
// is passed in, so tests run every rule with a fake one; winhttp_transport()
// is the real one (WinHTTP, TLS by default). Shared by Xerces.exe and
// `xerces_cli crash send`, so both send the same way.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crash_report.hpp"

namespace xerces_core {
// ------------------------------------------------------------- transport
struct http_request {
  std::string method; // GET, POST, DELETE
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  int timeout_ms = 15000; // each step of the exchange
};
struct http_response {
  bool reached = false; // an answer came back (any status)
  int status = 0;
  std::string body;
  std::optional<int> retry_after; // seconds, from Retry-After
  std::string error;              // when it did not reach the server
};
using crash_transport = std::function<http_response(const http_request &)>;

// WinHTTP: https:// with the system's TLS, or http:// to a local address
// only (127.0.0.1, ::1, localhost), for tests against a local backend. Any
// other host than crash_default_base's and the local ones is refused.
// timeout_ms applies to each step (name lookup, connect, send, answer).
http_response winhttp_send(const http_request &request);

// gzip (RFC 1952) of bytes, through zlib; empty on failure.
std::string gzip_bytes(std::string_view bytes);

// ---------------------------------------------------------------- where
// The base URL: XERCES_CRASH_URL when it is set, else crash_default_base;
// without a trailing '/'. The override is for tests against a backend on this
// PC: http:// or https:// to 127.0.0.1, localhost or [::1] only. nullopt when
// it is set to anything else (another host, user info): nothing is sent then.
std::optional<std::string> crash_base_url();
// Same rule for a given value (tests).
std::optional<std::string>
crash_base_url_from(std::optional<std::string> value);

// <home>\config\install_id: 32 random hex digits, made on first need from
// the system's random source, never from hardware or accounts. Empty when
// it can neither be read nor written, or (create false) when there is none:
// an install without one never sent a report.
std::string crash_install_id(const std::filesystem::path &home,
                             bool create = true);

// ---------------------------------------------------------------- outbox
inline constexpr std::size_t outbox_max_reports = 20;
inline constexpr std::int64_t outbox_max_age_seconds = 7 * 24 * 3600;

struct outbox_entry {
  std::string report_id;
  std::filesystem::path file;
  std::uint64_t size = 0;
  std::int64_t queued = 0; // seconds since 1970 (the file's write time)
};
// <crash>\outbox\<report_id>.json, oldest first.
std::vector<outbox_entry> list_outbox(const std::filesystem::path &crash_dir);
// Drops reports older than max_age, then all but the newest max_count.
// Returns how many went.
std::size_t prune_outbox(const std::filesystem::path &crash_dir,
                         std::int64_t now,
                         std::size_t max_count = outbox_max_reports,
                         std::int64_t max_age = outbox_max_age_seconds);
// Writes a report body to the outbox and prunes it.
bool enqueue_report(const std::filesystem::path &crash_dir,
                    std::string_view report_id, std::string_view body,
                    std::int64_t now);
// Removes every queued report (the setting was turned off).
std::size_t clear_outbox(const std::filesystem::path &crash_dir);

// ----------------------------------------------------- building reports
// What a report needs besides the crash: read from <home> and the game.
struct crash_context {
  std::filesystem::path home;
  std::string version;
  std::string commit;
  bool dirty = false;
  std::optional<std::uint32_t> game_checksum;
  std::optional<std::uint64_t> steam_build;
  std::string profile;   // %USERPROFILE%
  std::string user_name; // %USERNAME%
  // The launcher config's text (flags and redaction words); empty: none.
  std::string config_text;
  // The journal as it was when the game exited; empty: read the file.
  std::string journal_text;
  // collect_crash_system(), or a fixed one in tests.
  std::function<crash_system()> system;
  std::string now_utc;
  std::int64_t now = 0;
};

// The client's pending crashes in <crash>\pending: each .json with its
// .dmp (when there is one), oldest first.
struct pending_entry {
  pending_crash crash;
  std::filesystem::path json_file;
  std::filesystem::path dump_file; // empty when the dump is missing
};
std::vector<pending_entry> list_pending(const std::filesystem::path &crash_dir);

// A report from a pending crash (the dump, the console and the game log it
// names) or, without one, from the game's exit code. Both read the journal,
// the plan it names and the newest launcher log under ctx.home.
std::string report_from_pending(const pending_entry &pending,
                                std::optional<std::uint32_t> exit_code,
                                const crash_context &ctx);
std::string report_from_exit(std::string_view report_id,
                             std::uint32_t exit_code, const crash_context &ctx);

// After the game the launcher started has exited (or at a start, without
// an exit code): moves every pending crash into the outbox as a report, or,
// when there is none and the exit was a crash (exit_is_crash), makes one
// from the exit code. With sending off nothing is queued, and the pending
// crashes and the outbox are emptied, so a crash from that time is never
// sent later. Returns the report ids queued.
struct collect_result {
  std::vector<std::string> queued;
  std::size_t dropped = 0; // pending crashes and queued reports removed (off)
  bool crashed = false;    // the exit (when given) counted as a crash
};
collect_result collect_crashes(const crash_context &ctx, bool send_enabled,
                               std::optional<std::uint32_t> exit_code);

// ---------------------------------------------------------------- sending
struct send_options {
  std::string base_url;
  std::string install_id;
  // When install_id is empty: makes it (crash_install_id(home)) once the
  // server has said it takes crash reports, so an install none of whose
  // reports reached such a server has no id. An empty result: off.
  std::function<std::string()> make_install_id;
  std::string user_agent; // Xerces/<version>
  bool gzip = true;
  // Asked before every report, so turning the setting off stops a send that
  // is under way.
  std::function<bool()> enabled;
  // Seconds since 1970 for the outbox's age limit; 0: the clock.
  std::int64_t now = 0;
};

enum class send_outcome {
  sent,      // 201
  duplicate, // 200: the server had it already
  rejected,  // 400, 413 (even without the dump) or 422: removed, never sent
  retry,     // no answer, 429, 5xx, anything else: kept for later
  off,       // sending is off: kept, not sent
  not_taken, // the server does not take crash reports yet: kept, not sent
};
const char *to_string(send_outcome outcome);

// GET {base}/v1/crashes, asked before a run sends anything: whether the
// server takes crash reports. 404 means it does not (yet): that server has
// no crash routes, as the backend had none before they were deployed, so a
// report would only be turned away. A 2xx, or 405 (the route is there, for
// POST and DELETE only), means it does. Anything else, or no answer: ask
// again later. Only the User-Agent header goes with it.
enum class service_answer { takes_reports, not_yet, no_answer };
service_answer ask_service(const send_options &options,
                           const crash_transport &transport,
                           http_response *response = nullptr);

struct send_result {
  std::string report_id;
  send_outcome outcome = send_outcome::retry;
  int status = 0;
  std::string server_id;
  std::optional<int> retry_after;
  std::string error;
};

// Sends the outbox, oldest first, after pruning it (prune_outbox): a report
// older than 7 days is removed, never sent. Before the first report it asks
// the server whether it takes crash reports (ask_service): when it does not
// yet, or does not answer, nothing is sent and the one result says so
// (not_taken, retry). A report the server took (or had) is removed; one it
// refused is removed; on anything else the rest wait for the next try (the
// first failure stops the run). A 413 is tried once more without the
// minidump.
std::vector<send_result> send_outbox(const std::filesystem::path &crash_dir,
                                     const send_options &options,
                                     const crash_transport &transport);

// Whether a run left reports for a later try: one waits (retry, off,
// not_taken), or nothing was tried. Not after reports were only sent or
// refused.
bool reports_wait(const std::vector<send_result> &results);

// DELETE {base}/v1/crashes: every report of this install. The count the
// server deleted, or nullopt with error filled in.
std::optional<std::int64_t>
delete_remote_reports(const send_options &options,
                      const crash_transport &transport,
                      std::string *error = nullptr);
} // namespace xerces_core
