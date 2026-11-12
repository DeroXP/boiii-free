#pragma once

// Crash reports (docs/CRASH_REPORTS.md, DESIGN.md 5.10, D21): what the game
// client leaves behind when it crashes, how a crash is told apart from a
// normal exit, and the report the launcher sends to the Xerces backend when
// the "Send crash reports" setting is on. Everything here is pure except
// collect_crash_system() and new_report_id(), so the contract's field names,
// the redaction and the caps are tested.
//
// The client writes <home>\crash\pending\<report_id>.dmp (a stacks-only
// minidump) and <report_id>.json (pending_crash) from its crash handler. The
// launcher (or `xerces_cli crash send`) turns each into a report in
// <home>\crash\outbox\<report_id>.json (crash_outbox.hpp) and sends it.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core {
inline constexpr std::string_view crash_schema = "xerces.crash/1";
// The Xerces backend on Railway. XERCES_CRASH_URL overrides it, for local
// tests only (crash_outbox.hpp, crash_base_url).
inline constexpr std::string_view crash_default_base =
    "https://boiii-free-production.up.railway.app";
// Where testers report a crash by hand when sending is off.
inline constexpr std::string_view crash_issues_url =
    "https://github.com/DeroXP/boiii-free/issues";

// Caps from the contract: names and other short strings, each tail field,
// the raw minidump, frames, and the lines kept of each log.
inline constexpr std::size_t crash_name_cap = 256;
inline constexpr std::size_t crash_tail_cap = 64 * 1024;
inline constexpr std::size_t crash_minidump_cap = 2 * 1024 * 1024;
inline constexpr std::size_t crash_frame_cap = 32;
inline constexpr std::size_t crash_game_log_lines = 200;
inline constexpr std::size_t crash_launcher_log_lines = 100;
inline constexpr std::size_t crash_console_lines = 200;
// The client's text report as its note carries it (the report sends at most
// crash_tail_cap of it).
inline constexpr std::size_t crash_details_cap = 256 * 1024;

enum class crash_kind {
  exception,     // the client's handler ran and wrote the small dump
  fail_fast,     // exit code 0xC0000409, no dump
  out_of_memory, // exit 0xFFFFFFFF after the out-of-memory box
  unknown_exit,  // any other end that was not clean
};
const char *to_string(crash_kind kind);

// "0xC0000005": eight upper-case hex digits, as the dialog prints codes.
std::string crash_code_hex(std::uint32_t code);
// "0x14224FE5C": upper-case hex without padding, for offsets.
std::string crash_offset_hex(std::uint64_t offset);
// "BlackOps3.exe+0x14224FE5C".
std::string crash_frame(std::string_view module, std::uint64_t offset);

// 32 lower-case hex digits from the system's random source (BCrypt): report
// ids and the install id.
std::string new_report_id();
// 32 lower-case hex digits and nothing else.
bool valid_report_id(std::string_view id);

// What the client's crash handler writes next to its stacks-only dump, or
// its fatal-error dialog without a dump (kind unknown_exit).
struct pending_crash {
  std::string report_id;
  std::string created_utc;
  crash_kind kind = crash_kind::exception;
  std::uint32_t code = 0;              // the exception code
  std::string module;                  // empty when the address is in no module
  std::optional<std::uint64_t> offset; // in the module's image space
  std::string thread;                  // main, server, auxiliary
  std::vector<std::string> frames;     // module+0xoffset, at most 32
  std::string phase;                   // the journal's phase at the crash
  std::vector<std::string> console;    // the console's last lines
  std::vector<std::string> flags;      // the game's own flags (names only)
  std::string game_log;                // file name of the game's session log
  std::string journal;                 // the journal as it was (JSON text)
  // The player names the game knew (its own, the players it was in a match
  // with): words the report hides. Never sent.
  std::vector<std::string> names;
  // What the report is about, a line each (2026-11-12): why it was written
  // ("exception", "freeze", "fatal_error"), what happened in words, and the
  // script that was running. And the client's text report, the one it leaves
  // in the minidumps folder, without the console's lines: what happened,
  // what the game was doing, the stacks, memory and settings.
  std::string reason;
  std::string summary;
  std::string script;
  std::string details;
};
std::string pending_crash_to_json(const pending_crash &value);
std::optional<pending_crash> parse_pending_crash(std::string_view text);

// The flag names before the first "+" command ("-xerces-db-pool"); a flag's
// value (the plan id after -xerces-plan) is left out.
std::vector<std::string>
launch_flag_names(const std::vector<std::string> &args);

// The game the launcher started has exited: was it a crash? A new dump from
// the client always is; otherwise an exit code other than 0 with a journal
// that does not end in clean_exit. Exit code 0 is the crash dialog's restart
// or a quit before Xerces' code ran: not a crash without a dump.
bool exit_is_crash(std::uint32_t exit_code, bool journal_clean,
                   bool client_dump);

// The kind of a crash the client wrote no dump for. out_of_memory_logged:
// the game's log has loader.out_of_memory (the box Xerces shows).
crash_kind classify_exit(std::uint32_t exit_code, bool out_of_memory_logged);

// The report's "crash" object.
struct crash_facts {
  crash_kind kind = crash_kind::unknown_exit;
  std::string code; // hex
  std::optional<std::string> exit_code;
  std::optional<std::string> module, offset, thread;
  std::vector<std::string> frames;
  std::string phase;
  // From the client's note (pending_crash); none for a crash it wrote no
  // note for.
  std::optional<std::string> reason, summary, script;
};
crash_facts crash_from_pending(const pending_crash &crash,
                               std::optional<std::uint32_t> exit_code);
crash_facts crash_from_exit(std::uint32_t exit_code, bool out_of_memory_logged,
                            std::string phase);

struct crash_gpu {
  std::string name;
  std::string driver; // "32.0.15.8180"
};
struct crash_system {
  std::string windows; // "10.0.26300"
  std::string cpu;
  std::int64_t logical_cpus = 0;
  std::int64_t ram_mib = 0;
  std::vector<crash_gpu> gpus;
  std::string display; // "3840x2160@240"
};
// This PC: RtlGetVersion, the CPU name from the registry (CPUID when it has
// none), logical processors, RAM, the DXGI adapters with their driver
// versions (software adapters left out) and the primary display's mode.
crash_system collect_crash_system();
// A DXGI user-mode driver version (LARGE_INTEGER) as "a.b.c.d".
std::string format_driver_version(std::uint64_t version);

struct crash_mod {
  std::string id, name, version;
};
// The mods of a plan file (config\plans\<id>.json), main mod first, and its
// map's folder name when it has one.
struct plan_mods {
  std::vector<crash_mod> mods;
  std::optional<std::string> map;
};
plan_mods read_plan_mods(std::string_view plan_json);

// What a report is made from. Strings from logs and the console are scrubbed
// by build_crash_report (redact_crash_text) and cut to the caps.
struct crash_report_input {
  std::string report_id;
  std::string created_utc;
  std::string version;
  std::string commit;
  bool dirty = false;
  std::optional<std::uint32_t> game_checksum;
  std::optional<std::uint64_t> steam_build;
  crash_system system;
  std::string mode;
  std::optional<std::string> map;
  std::optional<std::string> plan_id;
  std::vector<crash_mod> mods;
  std::vector<std::string> flags;
  bool safe_mode = false;
  crash_facts crash;
  std::string journal_json;              // an object, or empty for null
  std::vector<std::string> game_log;     // lines of the game's JSON log
  std::vector<std::string> launcher_log; // lines of the launcher's log
  std::vector<std::string> console;      // the console's last lines
  std::string details;                   // the client's text report
  std::string minidump;                  // raw bytes; empty for none
  // Redaction: the profile folder, the Windows user name, and words to
  // hide (the player name, favourites' names, server addresses).
  std::string profile;
  std::string user_name;
  std::vector<std::string> player_names;
  std::vector<std::string> known_addresses;
};

// Scrubs text before it leaves the PC (reusing log.hpp's redaction, as the
// diagnostics bundle does): the profile folder becomes %USERPROFILE%, any
// other "X:\Users\<name>" C:\Users\<user>, the Windows user name <user>,
// player names <player>, Steam ids <steam id>, values after "xuid" <xuid>,
// and server addresses and favourites' names <address>. json: the text is
// JSON (or JSON Lines), whose strings are scrubbed decoded.
struct crash_redaction {
  std::string profile;
  std::string user_name;
  std::vector<std::string> player_names;
  std::vector<std::string> known_addresses;
};
std::string redact_crash_text(std::string_view text,
                              const crash_redaction &redaction, bool json);

// Every whole-word copy of a name (two characters or more, ASCII case
// ignored) becomes <player>; a colour code before it ("^1Name") still starts
// a word. The client runs this on the console lines it keeps.
std::string redact_player_names(std::string_view text,
                                const std::vector<std::string> &names);

// A stacks-only minidump as it may leave the PC. The name after every
// "X:\Users\" (UTF-16 module paths, ASCII PDB paths) is overwritten with
// '_', as the backend does; so is every copy of a word (the Windows user
// name, player names; three characters or more, ASCII case ignored, UTF-16
// or UTF-8) inside the module names, the PDB records and the memory the dump
// holds (the stacks). Lengths stay, so the dump still opens. The rest of the
// stack memory is raw: it can hold bits of what the game was doing. nullopt
// when the bytes are not a minidump.
std::optional<std::string> mask_minidump(std::string_view dump,
                                         const std::vector<std::string> &words);

// The last max_bytes of text, starting at a line, as valid UTF-8.
std::string tail_text(std::string_view text, std::size_t max_bytes);
// The first max_bytes of text, ending at a line, as valid UTF-8: the text
// report says what matters most first.
std::string head_text(std::string_view text, std::size_t max_bytes);

// The report body (contract xerces.crash/1), compact JSON.
std::string build_crash_report(const crash_report_input &input);

// What launcher.json says about sending, one rule for the launcher, the
// client and xerces_cli: no file (empty text) is the default, on; a file that
// is not JSON is off, since the person may have turned sending off in the
// file that broke; otherwise its send_crash_reports (on when missing).
// And the words the config gives for redaction: the player name,
// favourites' names and addresses, recent servers.
bool crash_reports_enabled(std::string_view launcher_json);
crash_redaction redaction_from_config(std::string_view launcher_json);

// The line the crash dialog adds about the report. started_by_launcher:
// XERCES_HOME is set, so the client wrote the pending report. send: the
// setting is on. launcher_open: Xerces.exe runs on this home. dump: the
// report has the stacks-only dump (a fatal error's has none).
std::string crash_report_note(bool started_by_launcher, bool send,
                              bool launcher_open, bool dump = true);
} // namespace xerces_core
