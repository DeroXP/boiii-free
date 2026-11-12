// What the game client relies on from the signature registry and its cache,
// and the pipe's hand-off to the main thread (docs/DESIGN.md 4.3, 15.2).

#include "harness.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include <xerces_core/handoff.hpp>
#include <xerces_core/sigregistry.hpp>
#include <xerces_core/sigruntime.hpp>
#include <xerces_core/sigscan.hpp>

using namespace xerces_core;
using namespace std::chrono_literals;

namespace {
std::vector<std::uint8_t> code(std::size_t size) {
  std::vector<std::uint8_t> bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    bytes[i] =
        static_cast<std::uint8_t>((i * 7 + 3) & 0x7f); // never 0xC3 or 0x8B
  }
  return bytes;
}

signature sig(std::string id, std::string pattern) {
  signature s;
  s.id = std::move(id);
  s.symbol = "Symbol";
  s.patterns = {std::move(pattern)};
  return s;
}

bool has(const std::vector<std::string> &items, std::string_view value) {
  return std::ranges::find(items, value) != items.end();
}
} // namespace

TEST_CASE("signature cache: the file name follows the registry text") {
  const std::string registry = R"({"schema":"xerces.signatures/1"})";
  const std::string name = signature_cache_name(0x6531394, registry);
  CHECK(name.starts_with("signatures-0x6531394-"));
  CHECK(name.ends_with(".json"));
  CHECK_EQ(name.size(), std::string("signatures-0x6531394-").size() + 16 +
                            std::string(".json").size());
  CHECK_EQ(signature_cache_name(0x6531394, registry), name);
  // Any edit to the registry, or another build, is another cache.
  CHECK(signature_cache_name(0x6531394, registry + " ") != name);
  CHECK(signature_cache_name(0x6531395, registry) != name);
}

TEST_CASE("signature cache: a hit must agree with the registry") {
  auto text = code(0x200);
  text[0x80] = 0x48;
  text[0x81] = 0x8B;
  text[0x82] = 0x8B;
  text[0x83] = 0xC3;
  const std::vector<section_view> sections = {{".text", 0x1000, text}};
  std::vector<signature> sigs = {sig("one", "48 8B 8B C3")};
  const auto cache = signature_cache_to_json(
      resolve_signatures(sigs, sections, 42), 42, sections);
  const auto cached = load_signature_cache(cache, 42, sections, sigs);
  REQUIRE(cached.has_value());
  std::string why;
  CHECK(cached_signatures_agree(*cached, sigs, 42, &why));
  CHECK(why.empty());

  // A known address for this build that matches is fine; one that does not
  // (a registry fixed after the cache was made) rejects the hit.
  sigs[0].known[42] = 0x1080;
  CHECK(cached_signatures_agree(*cached, sigs, 42, &why));
  sigs[0].known[42] = 0x1100;
  CHECK(!cached_signatures_agree(*cached, sigs, 42, &why));
  CHECK(why.find("known address") != std::string::npos);
  CHECK(why.find("0x1080") != std::string::npos);
  // Known addresses of other builds do not matter here.
  sigs[0].known.clear();
  sigs[0].known[7] = 0x1100;
  CHECK(cached_signatures_agree(*cached, sigs, 42, &why));

  // An entry the registry dropped, or one it added, rejects the hit too.
  CHECK(!cached_signatures_agree(*cached, {}, 42, &why));
  CHECK(why.find("no longer in the registry") != std::string::npos);
  auto more = sigs;
  more.push_back(sig("two", "C3"));
  CHECK(!cached_signatures_agree(*cached, more, 42, &why));
  CHECK(why.find("two") != std::string::npos);
}

TEST_CASE("signature registry: the shipped file has every multi-mod hook") {
  // mod_stack turns multi-mod off when one of its hook ids is missing or
  // unresolved; the registry it ships with must have them all, tagged
  // multi_mod, and nothing else tagged multi_mod.
  const auto file = xerces_tests::fixtures_dir().parent_path().parent_path() /
                    "compat" / "signatures.json";
  REQUIRE(std::filesystem::exists(file));
  const signature_registry registry =
      parse_signature_registry(xerces_tests::read_file(file));
  REQUIRE(registry.ok());
  for (const std::string_view id : multi_mod_signatures::all) {
    const auto entry = std::ranges::find_if(
        registry.signatures, [&](const signature &s) { return s.id == id; });
    CHECK(entry != registry.signatures.end());
    if (entry != registry.signatures.end()) {
      CHECK(has(entry->features, "multi_mod"));
    }
  }
  for (const auto &entry : registry.signatures) {
    if (has(entry.features, "multi_mod")) {
      CHECK(std::ranges::find(multi_mod_signatures::all, entry.id) !=
            multi_mod_signatures::all.end());
    }
  }
}

TEST_CASE("signature registry: the out-of-memory handler is its own feature") {
  // A miss of the handler turns only the out-of-memory report off, never
  // multi-mod; the client's detour never returns, so its pattern must name
  // exactly one known address on the build it was made from.
  const auto file = xerces_tests::fixtures_dir().parent_path().parent_path() /
                    "compat" / "signatures.json";
  REQUIRE(std::filesystem::exists(file));
  const signature_registry registry =
      parse_signature_registry(xerces_tests::read_file(file));
  REQUIRE(registry.ok());
  for (const std::string_view id : oom_report_signatures::all) {
    const auto entry = std::ranges::find_if(
        registry.signatures, [&](const signature &s) { return s.id == id; });
    REQUIRE(entry != registry.signatures.end());
    CHECK(entry->features == std::vector<std::string>{"oom_report"});
    CHECK(entry->kind == symbol_kind::function);
    CHECK(entry->known.contains(0x6531394u));
  }
  for (const auto &entry : registry.signatures) {
    if (has(entry.features, "oom_report")) {
      CHECK(std::ranges::find(oom_report_signatures::all, entry.id) !=
            oom_report_signatures::all.end());
    }
  }

  // Missing from a swapped registry: oom_report goes off, multi_mod stays on.
  signature_report report;
  for (const std::string_view id : multi_mod_signatures::all) {
    signature_result ok;
    ok.id = std::string(id);
    ok.ok = true;
    report.results.push_back(ok);
  }
  require_signatures(report, "multi_mod", multi_mod_signatures::all, "absent");
  require_signatures(report, "oom_report", oom_report_signatures::all,
                     "absent");
  CHECK(report.feature_enabled("multi_mod"));
  CHECK(!report.feature_enabled("oom_report"));
}

TEST_CASE("required signatures: all resolved leaves the feature on") {
  auto text = code(0x200);
  text[0x80] = 0x48;
  text[0x81] = 0x8B;
  text[0x82] = 0x8B;
  text[0x83] = 0xC3;
  const std::vector<section_view> sections = {{".text", 0x1000, text}};
  auto one = sig("one", "48 8B 8B C3");
  one.features = {"multi_mod"};
  signature_report report = resolve_signatures({one}, sections, 42);
  REQUIRE(report.find("one") != nullptr);
  REQUIRE(report.find("one")->ok);

  const std::array<std::string_view, 1> ids = {"one"};
  CHECK(require_signatures(report, "multi_mod", ids, "absent").empty());
  CHECK(report.feature_enabled("multi_mod"));
  // No empty list either: the launcher reads any "disabled" key as off.
  CHECK(report.disabled.find("multi_mod") == report.disabled.end());
  CHECK_EQ(report.results.size(), std::size_t(1));
}

TEST_CASE("required signatures: an id the registry lacks turns the feature "
          "off and is named") {
  // The registry resolved everything it has, so its tags alone say multi-mod
  // is on; the feature's own list says otherwise.
  signature_report report;
  signature_result found;
  found.id = "one";
  found.symbol = "One";
  found.ok = true;
  found.rva = 0x1080;
  report.results.push_back(found);
  REQUIRE(report.feature_enabled("multi_mod"));

  const std::array<std::string_view, 2> ids = {"one", "two"};
  const auto added = require_signatures(report, "multi_mod", ids,
                                        "not in compat/signatures.json");
  CHECK((added == std::vector<std::string>{"two"}));
  CHECK(!report.feature_enabled("multi_mod"));
  REQUIRE(report.disabled.count("multi_mod") == 1);
  CHECK((report.disabled.at("multi_mod") == std::vector<std::string>{"two"}));

  // The report gets a failed entry for it, so the log's miss lines and the
  // launcher's "Missing:" list name it, and resolved stays resolved.
  const signature_result *two = report.find("two");
  REQUIRE(two != nullptr);
  CHECK(!two->ok);
  CHECK_EQ(two->symbol, std::string("two"));
  CHECK_EQ(two->error, std::string("not in compat/signatures.json"));
  CHECK(report.find("one")->ok);
  CHECK_EQ(report.results.size(), std::size_t(2));

  const std::string message =
      report.message("multi_mod", "Multi-mod", "Single mods still work.");
  CHECK(message.starts_with("Multi-mod is off"));
  CHECK(message.find("two") != std::string::npos);
  CHECK(message.find("not in compat/signatures.json") != std::string::npos);

  // Applying it again changes nothing.
  CHECK(require_signatures(report, "multi_mod", ids, "absent").empty());
  CHECK_EQ(report.disabled.at("multi_mod").size(), std::size_t(1));
  CHECK_EQ(report.results.size(), std::size_t(2));
}

TEST_CASE("required signatures: a miss tagged for another feature turns this "
          "one off too") {
  // "label" is tagged mod_label in this registry and does not match: the scan
  // turns mod_label off, but multi_mod uses it as well.
  const auto text = code(0x200);
  const std::vector<section_view> sections = {{".text", 0x1000, text}};
  auto label = sig("label", "C3 C3 C3 C3");
  label.features = {"mod_label"};
  signature_report report = resolve_signatures({label}, sections, 42);
  REQUIRE(!report.find("label")->ok);
  REQUIRE(report.feature_enabled("multi_mod"));
  REQUIRE(!report.feature_enabled("mod_label"));

  const std::array<std::string_view, 1> ids = {"label"};
  const auto added = require_signatures(report, "multi_mod", ids, "absent");
  CHECK((added == std::vector<std::string>{"label"}));
  CHECK(!report.feature_enabled("multi_mod"));
  // The entry it already had is kept, with the scan's own error.
  CHECK_EQ(report.results.size(), std::size_t(1));
  CHECK(report.message("multi_mod", "Multi-mod", "").find("0 matches") !=
        std::string::npos);
}

TEST_CASE("required signatures: a miss tagged for the feature is listed once") {
  const auto text = code(0x200);
  const std::vector<section_view> sections = {{".text", 0x1000, text}};
  auto one = sig("one", "C3 C3 C3 C3");
  one.features = {"multi_mod"};
  signature_report report = resolve_signatures({one}, sections, 42);
  REQUIRE((report.disabled.at("multi_mod") == std::vector<std::string>{"one"}));

  const std::array<std::string_view, 1> ids = {"one"};
  CHECK(require_signatures(report, "multi_mod", ids, "absent").empty());
  CHECK((report.disabled.at("multi_mod") == std::vector<std::string>{"one"}));
}

TEST_CASE("required signatures: nothing resolved lists every id with the "
          "reason") {
  // No registry at all (or a broken one): the report is empty and each id the
  // feature needs is listed with why nothing resolved.
  signature_report report;
  const auto added =
      require_signatures(report, "multi_mod", multi_mod_signatures::all,
                         "compat/signatures.json was not found");
  CHECK_EQ(added.size(), multi_mod_signatures::all.size());
  CHECK(!report.feature_enabled("multi_mod"));
  CHECK_EQ(report.results.size(), multi_mod_signatures::all.size());
  for (const auto &result : report.results) {
    CHECK(!result.ok);
    CHECK_EQ(result.error, std::string("compat/signatures.json was not found"));
  }
}

TEST_CASE("handoff: a task that ran gives its result") {
  handoff call([] { return std::string("done"); });
  CHECK(call.run());
  CHECK(call.wait(0ms) == std::optional<std::string>("done"));
}

TEST_CASE("handoff: a task that had not started is dropped, never run late") {
  std::atomic<int> runs = 0;
  handoff call([&] {
    ++runs;
    return std::string("late");
  });
  CHECK(!call.wait(10ms).has_value());
  CHECK(!call.run()); // the main thread reaches it after the caller gave up
  CHECK_EQ(runs.load(), 0);
}

TEST_CASE("handoff: a task already running at the deadline is waited for") {
  std::atomic<bool> started = false;
  std::atomic<int> runs = 0;
  handoff call([&] {
    started = true;
    ++runs;
    std::this_thread::sleep_for(200ms); // still running when wait gives up
    return std::string("reloaded");
  });
  std::thread worker([&] { call.run(); });
  while (!started) {
    std::this_thread::yield();
  }
  // The deadline passes while the task runs: the answer is its real result,
  // not "busy".
  const auto result = call.wait(10ms);
  worker.join();
  CHECK(result == std::optional<std::string>("reloaded"));
  CHECK_EQ(runs.load(), 1);
}

TEST_CASE("handoff: a wait that is called off does not wait out its time") {
  // The game's main thread stops taking tasks when the game quits: the pipe's
  // thread must not keep the exit waiting for two seconds
  std::atomic<int> runs = 0;
  std::atomic<int> asked = 0;
  handoff call([&] {
    ++runs;
    return std::string("late");
  });
  const auto before = std::chrono::steady_clock::now();
  const auto result = call.wait(
      5000ms, [&] { return ++asked >= 3; }, 10ms);
  const auto took = std::chrono::steady_clock::now() - before;
  CHECK(!result.has_value());
  CHECK(asked.load() >= 3);
  CHECK(took < 2000ms);
  CHECK(!call.run()); // told "busy": it never runs
  CHECK_EQ(runs.load(), 0);

  // Called off before the first look: no waiting at all (the step is as long
  // as the timeout, so one step of waiting would show)
  handoff other([] { return std::string("x"); });
  const auto start = std::chrono::steady_clock::now();
  CHECK(!other.wait(5000ms, [] { return true; }, 5000ms).has_value());
  CHECK(std::chrono::steady_clock::now() - start < 2000ms);
  CHECK(!other.run());
}

TEST_CASE("handoff: a wait that can be called off still gives the result") {
  // Nobody calls it off and the task runs: its result, as with wait(timeout)
  handoff call([] { return std::string("done"); });
  std::thread worker([&] {
    std::this_thread::sleep_for(30ms);
    call.run();
  });
  const auto result = call.wait(5000ms, [] { return false; }, 5ms);
  worker.join();
  CHECK(result == std::optional<std::string>("done"));

  // Its time runs out with nobody calling it off: dropped, never run late
  handoff slow([] { return std::string("late"); });
  CHECK(!slow.wait(20ms, [] { return false; }, 5ms).has_value());
  CHECK(!slow.run());

  // No function to ask is the same as never calling it off
  handoff plain([] { return std::string("ok"); });
  CHECK(plain.run());
  CHECK(plain.wait(0ms, std::function<bool()>{}) ==
        std::optional<std::string>("ok"));
}

TEST_CASE("handoff: called off while the task runs, the task is waited for") {
  std::atomic<bool> started = false;
  handoff call([&] {
    started = true;
    std::this_thread::sleep_for(150ms);
    return std::string("reloaded");
  });
  std::thread worker([&] { call.run(); });
  while (!started) {
    std::this_thread::yield();
  }
  // What the task does is happening: the answer is its result, not "busy"
  const auto result = call.wait(5000ms, [] { return true; });
  worker.join();
  CHECK(result == std::optional<std::string>("reloaded"));
}

TEST_CASE("handoff: the task's exception reaches the waiter") {
  handoff call([]() -> std::string { throw std::runtime_error("broken"); });
  CHECK(call.run());
  bool thrown = false;
  try {
    (void)call.wait(0ms);
  } catch (const std::runtime_error &e) {
    thrown = std::string(e.what()) == "broken";
  }
  CHECK(thrown);
}
