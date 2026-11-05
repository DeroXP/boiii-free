// Review findings (docs/DESIGN.md 5.1-5.3, 6.1-6.9, 8.1-8.3, 15.1-15.2).
// Every test here fails against the code it was written against; each one
// names the defect it shows.

#include "harness.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <zstd.h>

#include <xerces_core/catalog.hpp>
#include <xerces_core/conflicts.hpp>
#include <xerces_core/fastfile_header.hpp>
#include <xerces_core/hooks.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/log.hpp>
#include <xerces_core/manifest.hpp>
#include <xerces_core/merge.hpp>
#include <xerces_core/pack.hpp>
#include <xerces_core/planner.hpp>
#include <xerces_core/semver.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <malloc.h>
#include <Windows.h>

using namespace xerces_core;
namespace fs = std::filesystem;

namespace {
const std::string zero_hash = "sha256:" + std::string(64, '0');
const std::string pack_id = "7b1e2c4a-1f0d-4f5e-9a51-3c2f8d0e9b11";

installed_content content_of(const std::string &manifest_json,
                             const std::string &folder = {}) {
  const manifest_result parsed = parse_manifest(manifest_json);
  for (const auto &d : parsed.diags.items()) {
    if (d.level == severity::error) {
      throw std::runtime_error("bad review manifest: " + d.path + " " +
                               d.message);
    }
  }
  installed_content content;
  content.info = parsed.value;
  content.folder = folder.empty() ? content.info.id : folder;
  content.content_hash = zero_hash;
  return content;
}

std::string mod_json(const std::string &id, const std::string &version,
                     const std::string &extra = {}) {
  return R"({"schema":1,"id":")" + id + R"(","name":")" + id +
         R"(","version":")" + version + "\"" + extra + "}";
}

plan_request request_for(std::vector<std::string> mods) {
  plan_request request;
  request.mode = "zm";
  request.mods = std::move(mods);
  request.game_build = "0x6531394";
  request.xerces_version = *version::parse("1.0.0");
  return request;
}

std::string version_of(const plan &value, const std::string &id) {
  for (const auto &entry : value.stack) {
    if (entry.id == id) {
      return entry.ver.to_string();
    }
  }
  return "(not in plan)";
}

manifest named(const std::string &id, bool legacy,
               std::vector<asset_ref> overrides = {}) {
  manifest m;
  m.id = id;
  m.name = id;
  m.legacy = legacy;
  m.overrides = std::move(overrides);
  return m;
}

plan stack_plan(const std::vector<std::string> &ids) {
  plan p;
  for (const auto &id : ids) {
    plan_entry entry;
    entry.id = id;
    entry.name = id;
    p.stack.push_back(entry);
  }
  return p;
}

asset_record script_record(const std::string &name, const std::string &hash) {
  return {"scriptparsetree", name, "core_mod.ff", 100, hash};
}

const asset_ownership *entry_named(const conflict_report &report,
                                   const std::string &name) {
  const auto it = std::ranges::find_if(
      report.entries, [&](const auto &e) { return e.name == name; });
  return it == report.entries.end() ? nullptr : &*it;
}

fs::path fresh_dir(const std::string &name) {
  const fs::path dir = fs::temp_directory_path() / ("xerces_review_" + name);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void write_text(const fs::path &path, const std::string &text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string share_code_of_json(const std::string &text) {
  std::string compressed(ZSTD_compressBound(text.size()), '\0');
  compressed.resize(ZSTD_compress(compressed.data(), compressed.size(),
                                  text.data(), text.size(), 19));
  return std::string(share_code_prefix) + base64url_encode(compressed);
}

// Runs fn(arg) on a thread with a 1 MiB stack (the size Windows gives a
// program's main thread) and reports whether it overflowed that stack.
struct guarded_call {
  void (*fn)(void *) = nullptr;
  void *arg = nullptr;
  bool overflowed = false;
};

int stack_overflow_filter(unsigned long code) {
  return code == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_EXECUTE_HANDLER
                                          : EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI guarded_thread(LPVOID raw) {
  auto *call = static_cast<guarded_call *>(raw);
  ULONG guarantee = 64 * 1024;
  SetThreadStackGuarantee(&guarantee);
  __try {
    call->fn(call->arg);
  } __except (stack_overflow_filter(GetExceptionCode())) {
    call->overflowed = true;
    _resetstkoflw();
  }
  return 0;
}

bool overflows_default_stack(void (*fn)(void *), void *arg) {
  guarded_call call;
  call.fn = fn;
  call.arg = arg;
  HANDLE thread = CreateThread(nullptr, 1024 * 1024, guarded_thread, &call,
                               STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (thread == nullptr) {
    throw std::runtime_error("CreateThread failed");
  }
  WaitForSingleObject(thread, INFINITE);
  CloseHandle(thread);
  return call.overflowed;
}

std::string joined(const std::vector<std::string> &items) {
  std::string out;
  for (const auto &item : items) {
    out += (out.empty() ? "" : ", ") + item;
  }
  return out;
}

// Runs the xerces_cli.exe built next to this test program; stdout and stderr
// together.
struct cli_result {
  unsigned long exit_code = 0;
  std::string out;
};

cli_result run_cli(const std::vector<std::wstring> &args) {
  wchar_t module[MAX_PATH]{};
  GetModuleFileNameW(nullptr, module, MAX_PATH);
  const fs::path cli = fs::path(module).parent_path() / L"xerces_cli.exe";
  if (!fs::exists(cli)) {
    throw std::runtime_error("xerces_cli.exe is not built next to the tests");
  }
  std::wstring command = L"\"" + cli.wstring() + L"\"";
  for (const auto &arg : args) {
    command += L" \"" + arg + L"\"";
  }
  SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &inherit, 0)) {
    throw std::runtime_error("CreatePipe failed");
  }
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_end;
  startup.hStdError = write_end;
  PROCESS_INFORMATION process{};
  const BOOL started =
      CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  CloseHandle(write_end);
  if (!started) {
    CloseHandle(read_end);
    throw std::runtime_error("CreateProcessW failed");
  }
  cli_result result;
  char buffer[4096];
  DWORD got = 0;
  while (ReadFile(read_end, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
    result.out.append(buffer, got);
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(process.hProcess, &code);
  result.exit_code = code;
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  CloseHandle(read_end);
  return result;
}
} // namespace

// ---------------------------------------------------------------- semver --

TEST_CASE("review: semver >1.2 and >1 do not admit prereleases of the bound") {
  // npm desugars ">1.2" to ">=1.3.0" and ">1" to ">=2.0.0" (no -0), so
  // 1.3.0-alpha and 2.0.0-beta satisfy neither (semver.satisfies is false).
  const auto minor = version_range::parse(">1.2");
  REQUIRE(minor.has_value());
  CHECK(!minor->satisfied_by(*version::parse("1.3.0-alpha")));
  const auto major = version_range::parse(">1");
  REQUIRE(major.has_value());
  CHECK(!major->satisfied_by(*version::parse("2.0.0-beta")));
  // Same bound written out: these already behave.
  CHECK(!version_range::parse(">=1.3.0")->satisfied_by(
      *version::parse("1.3.0-alpha")));
}

// --------------------------------------------------------------- planner --

TEST_CASE(
    "review: planner settles on satisfiable ranges that it oscillates on") {
  // Valid answers exist: x 1.0.0 with y 2.0.0, or x 1.0.0 with y 1.0.0.
  // The fixed-point loop cycles x2/y1 -> x1/y1 -> x1/y2 -> x2/y1 and gives up.
  const std::vector<installed_content> catalog = {
      content_of(
          mod_json("r.mod", "1.0.0",
                   R"(,"dependencies":[{"id":"x.mod"},{"id":"y.mod"}])")),
      content_of(
          mod_json("x.mod", "2.0.0",
                   R"(,"dependencies":[{"id":"y.mod","version":"^1"}])")),
      content_of(mod_json("x.mod", "1.0.0")),
      content_of(mod_json("y.mod", "2.0.0")),
      content_of(
          mod_json("y.mod", "1.0.0",
                   R"(,"dependencies":[{"id":"x.mod","version":"^1"}])")),
  };
  const plan result = resolve_plan(catalog, request_for({"r.mod"}));
  CHECK(!result.diags.has("plan.dependency.unstable"));
  CHECK(result.ok());
  CHECK_EQ(version_of(result, "x.mod"), "1.0.0");
}

TEST_CASE("review: planner resolves an acyclic chain deeper than 16 rounds") {
  // root depends on m01..m20, listed bottom-up. m(i) 2.0.0 needs m(i-1) ^1,
  // m(i) 1.0.0 needs nothing. Answer: m20 2.0.0, m19 1.0.0, m18 2.0.0, ...
  // There is no cycle anywhere, but each round fixes one more link, and the
  // loop stops after 16 rounds with plan.dependency.unstable.
  const auto id_of = [](int i) {
    return std::string("m") + (i < 10 ? "0" : "") + std::to_string(i) + ".mod";
  };
  std::string deps;
  for (int i = 1; i <= 20; ++i) {
    deps += std::string(i > 1 ? "," : "") + R"({"id":")" + id_of(i) + "\"}";
  }
  std::vector<installed_content> catalog = {content_of(
      mod_json("root.mod", "1.0.0", R"(,"dependencies":[)" + deps + "]"))};
  for (int i = 1; i <= 20; ++i) {
    catalog.push_back(content_of(mod_json(id_of(i), "1.0.0")));
    catalog.push_back(content_of(
        mod_json(id_of(i), "2.0.0",
                 i == 1 ? std::string{}
                        : R"(,"dependencies":[{"id":")" + id_of(i - 1) +
                              R"(","version":"^1"}])")));
  }
  const plan result = resolve_plan(catalog, request_for({"root.mod"}));
  CHECK(!result.diags.has("plan.dependency.unstable"));
  CHECK(result.ok());
  CHECK_EQ(version_of(result, "m20.mod"), "2.0.0");
  CHECK_EQ(version_of(result, "m19.mod"), "1.0.0");

  // Going back past twenty unrelated two-version mods: a 2.0.0 and b clash
  // over z, a 1.0.0 fits. Trying every combination of the twenty (2^20)
  // would run out of steps before reaching a 1.0.0.
  std::vector<installed_content> wide = {
      content_of(
          mod_json("a.mod", "2.0.0",
                   R"(,"dependencies":[{"id":"z.mod","version":"^1"}])")),
      content_of(
          mod_json("a.mod", "1.0.0",
                   R"(,"dependencies":[{"id":"z.mod","version":"^2"}])")),
      content_of(
          mod_json("b.mod", "1.0.0",
                   R"(,"dependencies":[{"id":"z.mod","version":"^2"}])")),
      content_of(mod_json("z.mod", "1.0.0")),
      content_of(mod_json("z.mod", "2.0.0"))};
  std::vector<std::string> mods = {"a.mod"};
  for (int i = 1; i <= 20; ++i) {
    wide.push_back(content_of(mod_json(id_of(i), "1.0.0")));
    wide.push_back(content_of(mod_json(id_of(i), "2.0.0")));
    mods.push_back(id_of(i));
  }
  mods.push_back("b.mod");
  const plan back = resolve_plan(wide, request_for(mods));
  CHECK(back.ok());
  CHECK_EQ(version_of(back, "a.mod"), "1.0.0");
  CHECK_EQ(version_of(back, "z.mod"), "2.0.0");
  CHECK_EQ(version_of(back, "m10.mod"), "2.0.0");
}

TEST_CASE("review: planner picks the same copy whatever the catalog order") {
  // The same mod installed twice (say a Workshop copy and a local copy):
  // same id, version and content, different folders. 6.8 says shuffled
  // inputs give byte-identical plans; here the first copy listed wins.
  installed_content first = content_of(mod_json("dup.mod", "1.0.0"), "dup_a");
  installed_content second = first;
  second.folder = "dup_b";
  const plan ab = resolve_plan({first, second}, request_for({"dup.mod"}));
  const plan ba = resolve_plan({second, first}, request_for({"dup.mod"}));
  CHECK_EQ(ab.fs_game(), ba.fs_game());
  CHECK_EQ(ab.plan_id, ba.plan_id);
}

TEST_CASE("review: planner does not call an installed pinned version missing") {
  // b.mod 1.0.0 is installed; it is pinned but a.mod needs ^2. The error says
  // "Version 1.0.0 of b.mod is not installed" and the fix is to install it.
  const std::vector<installed_content> catalog = {
      content_of(
          mod_json("a.mod", "1.0.0",
                   R"(,"dependencies":[{"id":"b.mod","version":"^2"}])")),
      content_of(mod_json("b.mod", "1.0.0")),
      content_of(mod_json("b.mod", "2.0.0"))};
  plan_request request = request_for({"a.mod", "b.mod"});
  request.pins["b.mod"] = "1.0.0";
  const plan result = resolve_plan(catalog, request);
  CHECK(!result.ok());
  CHECK(std::ranges::none_of(result.diags.items(), [](const diagnostic &d) {
    return d.message.find("is not installed") != std::string::npos;
  }));
  CHECK(std::ranges::none_of(result.fixes, [](const fix_action &f) {
    return f.kind == "install" && f.id == "b.mod";
  }));
}

TEST_CASE("review: a manifest that failed validation does not plan as ok") {
  // a.mod's dependency range "^^1" is rejected by parse_manifest, but the
  // dependency is kept with range "*", the mod stays in the catalog, and the
  // plan comes out ok() with b.mod 3.0.0 (plan resolve --json shows ok:true
  // and hides the scan errors).
  const fs::path dir = fresh_dir("invalid_manifest");
  write_text(dir / "a" / "xerces.mod.json",
             mod_json("a.mod", "1.0.0",
                      R"(,"dependencies":[{"id":"b.mod","version":"^^1"}])"));
  write_text(dir / "b" / "xerces.mod.json", mod_json("b.mod", "3.0.0"));
  const catalog_scan scan = scan_mods_directory(dir);
  CHECK(scan.diags.has("manifest.range.invalid"));
  const plan result = resolve_plan(scan.catalog, request_for({"a.mod"}));
  CHECK(!result.ok());
  fs::remove_all(dir);
}

// ----------------------------------------------------------------- merge --

TEST_CASE("review: csv blank lines are not rows") {
  // A trailing blank line (common from editors) becomes a row with one empty
  // cell: the merge counts it as an added row and writes an empty row.
  const auto base = parse_csv("a,1\nb,2\n");
  const auto mod = parse_csv("a,1\nb,2\nc,3\n\n");
  REQUIRE(base.has_value());
  REQUIRE(mod.has_value());
  CHECK_EQ(mod->rows.size(), 3u);
  const table_merge merged = merge_tables(*base, {{"m", *mod}}, {0});
  CHECK_EQ(write_csv(merged.merged), "a,1\nb,2\nc,3\n");
  CHECK_EQ(merged.stats.added.at("m"), 1u);
  // A blank line in the base that the mod's copy lacks is a "removed row".
  const table_merge removed =
      merge_tables(*parse_csv("a,1\n\nb,2\n"), {{"m", *base}}, {0});
  CHECK(removed.stats.removed.empty());
}

TEST_CASE("review: json merge reports two mods changing the same nested key") {
  // Both add "new" with a different "x": the higher wins (fine) but nothing is
  // reported, because only "/new" got an owner, not "/new/x".
  const text_merge added =
      merge_json_documents(std::string("{}"), {{"low", R"({"new":{"x":1}})"},
                                               {"high", R"({"new":{"x":2}})"}});
  CHECK(added.merged.find("\"x\": 2") != std::string::npos);
  CHECK_EQ(added.stats.conflicts.size(), 1u);
  if (added.stats.conflicts.size() == 1) {
    CHECK_EQ(added.stats.conflicts[0].key, "/new/x");
    CHECK_EQ(added.stats.conflicts[0].winner, "high");
  }

  // The higher mod replaces an object whose member the lower mod changed.
  const text_merge replaced = merge_json_documents(
      std::string(R"({"o":{"x":1}})"),
      {{"low", R"({"o":{"x":2}})"}, {"high", R"({"o":5})"}});
  CHECK_EQ(replaced.stats.conflicts.size(), 1u);
}

// ------------------------------------------------------------- conflicts --

TEST_CASE("review: a blocked override does not hide a legacy conflict below") {
  // new.mod (manifest, no declared override) is blocked; the two legacy mods
  // under it replace the same script with different code (rule 4). That
  // conflict needs a decision, but the entry stops at "blocked".
  const plan p = stack_plan({"new.mod", "legacy.one", "legacy.two"});
  conflict_inputs inputs;
  inputs.resolved = &p;
  inputs.manifests = {{"new.mod", named("new.mod", false)},
                      {"legacy.one", named("legacy.one", true)},
                      {"legacy.two", named("legacy.two", true)}};
  inputs.indexes = {
      {"new.mod", {script_record("scripts/zm/_zm_perks.gsc", "sha256:1")}, {}},
      {"legacy.one",
       {script_record("scripts/zm/_zm_perks.gsc", "sha256:2")},
       {}},
      {"legacy.two",
       {script_record("scripts/zm/_zm_perks.gsc", "sha256:3")},
       {}}};
  const conflict_report report = check_conflicts(inputs);
  const auto *entry = entry_named(report, "scripts/zm/_zm_perks.gsc");
  REQUIRE(entry != nullptr);
  CHECK_EQ(entry->winner, "legacy.one");
  CHECK_EQ(report.decisions, 1u);
  CHECK(report.needs_decision());
}

TEST_CASE("review: a mod whose id is \"stock\" is not taken for the game") {
  // "stock" is a valid mod id. As a winner it hits the winner == "stock"
  // branch, gets an empty hash, and an identical copy becomes a rule-4
  // conflict that needs a decision ("the game and b.mod both replace ...").
  CHECK(valid_mod_id("stock"));
  const plan p = stack_plan({"stock", "b.mod"});
  conflict_inputs inputs;
  inputs.resolved = &p;
  inputs.manifests = {{"stock", named("stock", true)},
                      {"b.mod", named("b.mod", true)}};
  inputs.indexes = {
      {"stock", {script_record("scripts/x.gsc", "sha256:x")}, {}},
      {"b.mod", {script_record("scripts/x.gsc", "sha256:x")}, {}}};
  const conflict_report report = check_conflicts(inputs);
  CHECK_EQ(report.identical, 1u);
  CHECK_EQ(report.decisions, 0u);
}

TEST_CASE("review: a copy identical to the game's is not an override") {
  // Rule 5: identical content is never a conflict. The stock index has the
  // same hash, but the stock hash is never compared, so an unchanged copy of
  // a stock script is "blocked" with an allow_override fix.
  const plan p = stack_plan({"m.mod"});
  conflict_inputs inputs;
  inputs.resolved = &p;
  inputs.manifests = {{"m.mod", named("m.mod", false)}};
  inputs.indexes = {
      {"m.mod", {script_record("scripts/zm/_zm.gsc", "sha256:same")}, {}}};
  inputs.stock = asset_index{
      "stock", {script_record("scripts/zm/_zm.gsc", "sha256:same")}, {}};
  const conflict_report report = check_conflicts(inputs);
  CHECK_EQ(report.blocked, 0u);
  CHECK(report.fixes.empty());
  const auto *entry = entry_named(report, "scripts/zm/_zm.gsc");
  CHECK(entry == nullptr || entry->outcome == asset_outcome::identical);
}

// ----------------------------------------------------------------- hooks --

TEST_CASE("review: after hooks unwind with the highest mod outermost") {
  // 6.6: the highest-priority mod's hook is outermost, so its after hook runs
  // last, once the inner links have returned.
  const plan p = stack_plan({"high.mod", "low.mod"});
  manifest high = named("high.mod", false);
  high.hooks = {
      {"scripts/a.gsc::f", "scripts/h.gsc::after_f", hook_kind::after}};
  manifest low = named("low.mod", false);
  low.hooks = {
      {"scripts/a.gsc::f", "scripts/l.gsc::after_f", hook_kind::after}};
  const hook_plan hooks = plan_hooks(p, {{"high.mod", high}, {"low.mod", low}});
  REQUIRE_EQ(hooks.chains.size(), 1u);
  CHECK_EQ(joined(simulate_chain(hooks.chains[0])),
           "original, after:low.mod, after:high.mod");
}

// ------------------------------------------------------------------ pack --

TEST_CASE(
    "review: a deeply nested share code is refused, not a stack overflow") {
  // A 164-character share code inflates to 200 KB of nested arrays, under the
  // 256 KiB limit. json::parse (rapidjson's recursive parser) and
  // scan_strings recurse once per level; 1 MiB of stack runs out between
  // 8,000 and 16,000 levels. xerces_cli packs validate <code> dies with
  // 0xC00000FD. The same pack nested 10 deep decodes fine.
  std::string text = R"({"schema":1,"id":")" + pack_id +
                     R"(","name":"x","mode":"zm","mods":[],"order":[],"x":)";
  text += std::string(100000, '[') + std::string(100000, ']') + "}";
  REQUIRE(text.size() < share_code_max_json);
  struct state {
    std::string code;
    bool ok = true;
  } s{share_code_of_json(text)};
  CHECK(s.code.size() < 200);
  const bool overflowed = overflows_default_stack(
      [](void *raw) {
        auto *st = static_cast<state *>(raw);
        st->ok = decode_share_code(st->code).ok();
      },
      &s);
  CHECK(!overflowed);
  if (!overflowed) {
    CHECK(!s.ok); // refused for its depth
  }
}

TEST_CASE(
    "review: exporting a pack with a long ws.<digits> id does not throw") {
  // valid_mod_id accepts "ws." + 24 digits; workshop_of() calls std::stoull
  // on the digits, which throws std::out_of_range.
  plan p;
  plan_entry entry;
  entry.id = "ws.123456789012345678901234";
  entry.name = "Big";
  entry.ver = *version::parse("1.0.0");
  entry.content_hash = zero_hash;
  p.stack.push_back(entry);
  CHECK(valid_mod_id(entry.id));
  bool threw = false;
  try {
    (void)pack_from_plan(p, "x", pack_id, {});
  } catch (const std::exception &) {
    threw = true;
  }
  CHECK(!threw);
}

TEST_CASE(
    "review: pack local-path check finds paths that are not at the start") {
  const std::string head =
      R"({"schema":1,"id":")" + pack_id +
      R"(","name":"x","mode":"zm","mods":[{"id":"a.b.c","version":"1.0.0",)"
      R"("content_hash":")" +
      zero_hash + R"(","settings":{"p":")";
  const std::string tail = R"("}}],"order":["a.b.c"]})";
  for (const char *value : {"music=D:\\\\Music\\\\bo3", "//nas/share/mods",
                            "file:///D:/Games/mods"}) {
    const pack_result result = parse_pack(head + value + tail);
    if (!result.diags.has("pack.local_path")) {
      xerces_tests::check_failed("pack.local_path", __FILE__, __LINE__,
                                 std::string("not flagged: ") + value);
    }
  }
}

TEST_CASE("review: pack names are limited to 80 characters, not 80 bytes") {
  // pack-1.schema.json: name maxLength 80 (characters). 50 Cyrillic letters
  // are 100 UTF-8 bytes and are refused.
  std::string name;
  for (int i = 0; i < 50; ++i) {
    name += "\xD0\x96";
  }
  const pack_result result =
      parse_pack(R"({"schema":1,"id":")" + pack_id + R"(","name":")" + name +
                 R"(","mode":"zm","mods":[],"order":[]})");
  CHECK(!result.diags.has("pack.name"));
}

// -------------------------------------------------------------- manifest --

TEST_CASE("review: a numeric PublisherID too big for 64 bits does not throw") {
  // synthesize_legacy calls std::stoull on any all-digit PublisherID; past
  // 2^64 it throws std::out_of_range, and scan_mods_directory loses every
  // mod in the folder, not just the bad one (xerces_cli: "error: stoull").
  legacy_input input;
  input.folder = "12345";
  input.workshop_json =
      R"({"PublisherID":"123456789012345678901234567890","Title":"Big"})";
  bool threw = false;
  try {
    (void)synthesize_legacy(input, {});
  } catch (const std::exception &) {
    threw = true;
  }
  CHECK(!threw);

  const fs::path dir = fresh_dir("bad_workshop_json");
  write_text(dir / "a_good" / "xerces.mod.json", mod_json("good.mod", "1.0.0"));
  write_text(dir / "b_bad" / "workshop.json", *input.workshop_json);
  bool scan_threw = false;
  try {
    const catalog_scan scan = scan_mods_directory(dir);
    CHECK_EQ(scan.catalog.size(), 2u);
  } catch (const std::exception &) {
    scan_threw = true;
  }
  CHECK(!scan_threw);
  fs::remove_all(dir);
}

TEST_CASE("review: a synthesized manifest keeps \"legacy\" through its JSON") {
  // manifest_to_json writes "legacy": true; parse_manifest does not read it,
  // warns that it is unknown, and the manifest stops being legacy (so rule 3
  // starts blocking its overrides).
  legacy_input input;
  input.folder = "1944930083";
  input.workshop_json =
      R"({"PublisherID":"1944930083","Title":"Solo","Type":"mod"})";
  input.time_updated = 1700000000;
  const manifest m = synthesize_legacy(input, {});
  REQUIRE(m.legacy);
  const manifest_result again = parse_manifest(manifest_to_json(m));
  CHECK(again.value.legacy);
  CHECK(!again.diags.has("manifest.field.unknown"));
}

TEST_CASE("review: asset types in a manifest match whatever their case") {
  // Names are normalized to lower case, types are not, and declares_override
  // compares exactly; strategy_for and has_exact_hash lower-case the type.
  const manifest_result result = parse_manifest(mod_json(
      "t.mod", "1.0.0",
      R"(,"assets":{"overrides":[{"type":"ScriptParseTree","name":"scripts/zm/a.gsc","reason":"r"}]},)"
      R"("scripts":{"hooks":[{"target":"scripts/zm/a.gsc::f","handler":"scripts/zm/b.gsc::g","kind":"replace"}]})"));
  CHECK(!result.diags.has("manifest.hook.replace_undeclared"));
  CHECK(result.value.declares_override("scriptparsetree", "scripts/zm/a.gsc"));
}

// --------------------------------------------------------------- catalog --

TEST_CASE("review: the folder for an id is the folder of the version planned") {
  // Two versions of one id side by side (catalog.cpp allows it). roots[id]
  // can only hold one folder (the one scanned last), so each version carries
  // its own absolute path, and the plan entry the path of the version it
  // picked; xerces_cli's conflict check reads files and the manifest from
  // there.
  const fs::path dir = fresh_dir("two_versions");
  write_text(dir / "dup_new" / "xerces.mod.json", mod_json("dup.mod", "2.0.0"));
  write_text(dir / "dup_new" / "scripts" / "zm" / "new.gsc", "// 2.0.0\n");
  write_text(dir / "dup_old" / "xerces.mod.json", mod_json("dup.mod", "1.0.0"));
  write_text(dir / "dup_old" / "scripts" / "zm" / "old.gsc", "// 1.0.0\n");
  const catalog_scan scan = scan_mods_directory(dir);
  REQUIRE_EQ(scan.catalog.size(), 2u);
  const plan result = resolve_plan(scan.catalog, request_for({"dup.mod"}));
  REQUIRE(result.ok());
  REQUIRE_EQ(result.stack.size(), 1u);
  CHECK_EQ(result.stack[0].ver.to_string(), "2.0.0");
  CHECK_EQ(result.stack[0].folder, "dup_new");
  CHECK(result.stack[0].path.is_absolute());
  CHECK_EQ(result.stack[0].path.filename().string(), result.stack[0].folder);
  const asset_index index =
      index_loose_files("dup.mod", read_files(result.stack[0].path));
  CHECK(std::ranges::any_of(index.records, [](const asset_record &r) {
    return r.name == "scripts/zm/new.gsc";
  }));
  fs::remove_all(dir);
}

// ------------------------------------------------------------------ json --

TEST_CASE("review: canonical JSON writes one number one way") {
  // Integral doubles below 9e15 print as integers, from 9e15 up in exponent
  // form, while the same value written as an integer literal prints in full.
  const auto as_double = json::parse("[9.5e15]");
  const auto as_integer = json::parse("[9500000000000000]");
  REQUIRE(as_double.ok);
  REQUIRE(as_integer.ok);
  CHECK_EQ(json::canonical(as_double.document),
           json::canonical(as_integer.document));
}

// ------------------------------------------------------------------- log --

TEST_CASE("review: logs redact a profile path with non-ASCII characters") {
  // The profile is read with _dupenv_s, which converts it to the ANSI code
  // page (1252 here: u-umlaut is one byte, 0xFC). Paths in xerces_core are
  // UTF-8 (u-umlaut is 0xC3 0xBC), so they never match and the name leaks.
  wchar_t *saved = nullptr;
  std::size_t saved_size = 0;
  _wdupenv_s(&saved, &saved_size, L"USERPROFILE");
  _wputenv_s(L"USERPROFILE", L"C:\\Users\\J\u00fcrgen");
  const json_log log("cli", "s1");
  _wputenv_s(L"USERPROFILE", saved != nullptr ? saved : L"");
  std::free(saved);

  log_event event;
  event.message = "reading C:\\Users\\J\xC3\xBCrgen\\mods"; // UTF-8
  const std::string line = log.format(event, "t");
  CHECK(line.find("rgen") == std::string::npos);
  CHECK(line.find("%USERPROFILE%") != std::string::npos);
}

// ------------------------------------------------------------------- cli --

TEST_CASE("review: cli conflicts check judges the version the plan launches") {
  // dup.mod 2.0.0 declares its override of _zm_perks.gsc; 1.0.0 does not.
  // plan resolve launches 2.0.0, but check() takes files from
  // scan.roots[id] and the manifest from a map keyed by id, both of which
  // hold 1.0.0 (scanned last), so the report blocks the override.
  const fs::path dir = fresh_dir("cli_two_versions");
  write_text(
      dir / "mods" / "dup_new" / "xerces.mod.json",
      mod_json(
          "dup.mod", "2.0.0",
          R"(,"assets":{"overrides":[{"type":"scriptparsetree","name":"scripts/zm/_zm_perks.gsc","reason":"perk limit"}]})"));
  write_text(dir / "mods" / "dup_new" / "scripts" / "zm" / "_zm_perks.gsc",
             "// 2.0.0\n");
  write_text(dir / "mods" / "dup_old" / "xerces.mod.json",
             mod_json("dup.mod", "1.0.0"));
  write_text(dir / "mods" / "dup_old" / "scripts" / "zm" / "_zm_perks.gsc",
             "// 1.0.0\n");
  write_text(dir / "stock" / "scripts" / "zm" / "_zm_perks.gsc", "// stock\n");

  const cli_result planned =
      run_cli({L"plan", L"resolve", L"--mods-dir", (dir / "mods").wstring(),
               L"--mod", L"dup.mod", L"--json"});
  CHECK(planned.out.find("\"version\": \"2.0.0\"") != std::string::npos);
  const cli_result checked = run_cli(
      {L"conflicts", L"check", L"--mods-dir", (dir / "mods").wstring(),
       L"--mod", L"dup.mod", L"--stock", (dir / "stock").wstring(), L"--json"});
  CHECK(checked.out.find("\"outcome\": \"override_allowed\"") !=
        std::string::npos);
  CHECK(checked.out.find("\"blocked\": 0") != std::string::npos);
  fs::remove_all(dir);
}

TEST_CASE("review: cli --map-last puts the map's scripts below the mods") {
  // "--map-last" is in flag_names but nothing reads it; without a pack there
  // is no other way to turn rule 6 off.
  const fs::path dir = fresh_dir("cli_map_last");
  write_text(dir / "zm_test" / "xerces.mod.json",
             mod_json("ws.map", "1.0.0", R"(,"kind":"map")"));
  write_text(dir / "zm_test" / "scripts" / "zm" / "zm_test.gsc", "// map\n");
  write_text(
      dir / "amod" / "xerces.mod.json",
      mod_json(
          "a.mod", "1.0.0",
          R"(,"assets":{"overrides":[{"type":"scriptparsetree","name":"scripts/zm/zm_test.gsc","reason":"r"}]})"));
  write_text(dir / "amod" / "scripts" / "zm" / "zm_test.gsc", "// mod\n");
  const cli_result result =
      run_cli({L"conflicts", L"check", L"--mods-dir", dir.wstring(), L"--mod",
               L"a.mod", L"--map", L"ws.map", L"--map-last", L"--json"});
  CHECK(result.out.find("\"winner\": \"a.mod\"") != std::string::npos);
  fs::remove_all(dir);
}

TEST_CASE("review: cli refuses a negative --stock-zones") {
  // std::stoul("-1") wraps to 4294967295 instead of failing, so the plan
  // reports "needs 4294967295 zone slots" (exit 2) rather than a bad
  // argument (exit 1, as "--stock-zones abc" gives).
  const fs::path dir = fresh_dir("cli_stock_zones");
  write_text(dir / "b" / "xerces.mod.json", mod_json("b.mod", "1.0.0"));
  const cli_result result =
      run_cli({L"plan", L"resolve", L"--mods-dir", dir.wstring(), L"--mod",
               L"b.mod", L"--stock-zones", L"-1"});
  CHECK_EQ(result.exit_code, 1ul);
  CHECK(result.out.find("4294967295") == std::string::npos);
  fs::remove_all(dir);
}

TEST_CASE("review: cli packs import and conflicts --pack check zone memory") {
  // Both rebuild the request with pack_to_request, which never set
  // zone_budget: a pack whose map does not fit passed its dry run, and the
  // zone memory line said there were no figures for the build.
  const fs::path dir = fresh_dir("cli_pack_zone_memory");
  const fs::path mods = dir / "workshop";
  // A fastfile header asking `mib` MiB of the DB stack's main pool.
  const auto ff = [](std::uint64_t mib) {
    std::string out(fastfile_header_bytes, '\0');
    std::memcpy(out.data(), fastfile_magic.data(), fastfile_magic.size());
    for (std::size_t i = 0; i < 4; ++i) {
      out[8 + i] = static_cast<char>((fastfile_version >> (8 * i)) & 0xFF);
    }
    const std::uint64_t bytes = mib * 1024 * 1024;
    for (std::size_t i = 0; i < 8; ++i) {
      out[fastfile_blocks_offset + 8 * 6 + i] =
          static_cast<char>((bytes >> (8 * i)) & 0xFF);
    }
    return out;
  };
  write_text(mods / "500" / "workshop.json",
             R"({"PublisherID":"500","Type":"mod","Title":"Heavy Mod"})");
  write_text(mods / "500" / "core_mod.ff", ff(500));
  write_text(mods / "600" / "workshop.json",
             R"({"PublisherID":"600","Type":"map","Title":"Huge Map"})");
  write_text(mods / "600" / "zm_huge.ff", ff(800));
  const fs::path figures = dir / "zone_memory.json";
  write_text(figures,
             R"({"schema":"xerces.zone_memory/1","builds":[{"name":"Test",)"
             R"("checksum":"0x7E570002","db_main_pool_mib":2048,)"
             R"("baseline_mib":{"zm":849}}]})");
  // Exported without a build, so nothing is checked and the export works.
  const fs::path pack_file = dir / "pack.json";
  const cli_result exported =
      run_cli({L"packs", L"export", L"--mods-dir", mods.wstring(), L"--name",
               L"Big", L"--mod", L"ws.500", L"--map", L"ws.600", L"--out",
               pack_file.wstring()});
  REQUIRE_EQ(exported.exit_code, 0ul);

  // 849 + 500 + 800 = 2149 MiB, 101 over.
  const cli_result imported = run_cli(
      {L"packs", L"import", pack_file.wstring(), L"--mods-dir", mods.wstring(),
       L"--game-build", L"0x7E570002", L"--zone-memory", figures.wstring()});
  CHECK_EQ(imported.exit_code, 2ul);
  CHECK(imported.out.find("zone memory: 2149 of 2048 MiB with the map") !=
        std::string::npos);
  CHECK(imported.out.find("(plan.memory.over)") != std::string::npos);
  CHECK(imported.out.find("no figures") == std::string::npos);

  // The pack's own mode counts: --mode mp (no figures) does not skip it.
  const cli_result conflicts =
      run_cli({L"conflicts", L"check", L"--mods-dir", mods.wstring(), L"--pack",
               pack_file.wstring(), L"--mode", L"mp", L"--game-build",
               L"0x7E570002", L"--zone-memory", figures.wstring()});
  CHECK_EQ(conflicts.exit_code, 2ul);
  CHECK(conflicts.out.find("(plan.memory.over)") != std::string::npos);
  fs::remove_all(dir);
}

TEST_CASE("review: cli accepts a share code with surrounding whitespace") {
  // decode_share_code trims whitespace, but load_pack checks the "XRC1."
  // prefix first, so " XRC1..." is looked up as a file that does not exist.
  pack value;
  value.id = pack_id;
  value.name = "Weekend";
  const std::string code = encode_share_code(value);
  REQUIRE(decode_share_code(" " + code + "\n").ok());
  const std::wstring padded = L" " + std::wstring(code.begin(), code.end());
  const cli_result result = run_cli({L"packs", L"validate", padded});
  CHECK_EQ(result.exit_code, 0ul);
  CHECK(result.out.find("does not exist") == std::string::npos);
}
