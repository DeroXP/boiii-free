// What the game client relies on from the signature registry and its cache,
// and the pipe's hand-off to the main thread (docs/DESIGN.md 4.3, 15.2).

#include "harness.hpp"

#include <algorithm>
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
