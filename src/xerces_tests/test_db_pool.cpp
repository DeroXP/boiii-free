// The opt-in larger zone-memory pool (docs/DESIGN.md 5.4 step 4): what the
// client's detours decide and log, the launch flag from the launcher's
// setting to the client's check, and the registry entries they rely on.

#include "harness.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <xerces_core/db_pool.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/launch.hpp>
#include <xerces_core/settings_options.hpp>
#include <xerces_core/sigregistry.hpp>
#include <xerces_core/sigruntime.hpp>

using namespace xerces_core;
namespace pool = xerces_core::db_pool;

namespace {
pool::pool_call call(const char *name, std::uint32_t flags, std::uint64_t size,
                     std::uint64_t given, std::uint64_t result = 1) {
  pool::pool_call c;
  c.name = pool::copy_pool_name(name);
  c.flags = flags;
  c.size_in = size;
  c.size_out = given;
  c.result = result;
  return c;
}

// What build 0x6531394 does: the DB main pool first, then the rest.
pool::report applied_report() {
  pool::report r;
  r.calls = {call("main", 0x1E, 0x80000000, 0x100000000),
             call("gpu", 0x3E, 0x100000000, 0x100000000),
             call("main", 0x1E, 0x80000000, 0x80000000)};
  r.pool_calls = 23;
  r.budget = pool::budget_call{0x140000000, 0x200000000, 0};
  r.budget_calls = 1;
  r.main_size = 0x100000000;
  r.main_pages = 19010;
  return r;
}
} // namespace

TEST_CASE("db pool: only the first call, the stock DB main pool, grows") {
  CHECK_EQ(pool::pool_size(true, "main", 0x1E, 0x80000000),
           std::uint64_t{0x100000000});
  // Five more "main" pools of 2 GiB with the same flags come later (other
  // stacks): they keep their size.
  CHECK_EQ(pool::pool_size(false, "main", 0x1E, 0x80000000),
           std::uint64_t{0x80000000});
  // A first call that is anything else is left alone.
  CHECK_EQ(pool::pool_size(true, "gpu", 0x1E, 0x80000000),
           std::uint64_t{0x80000000});
  CHECK_EQ(pool::pool_size(true, "main", 0x3E, 0x80000000),
           std::uint64_t{0x80000000});
  CHECK_EQ(pool::pool_size(true, "main", 0x1E, 0x10000000),
           std::uint64_t{0x10000000});
  CHECK_EQ(pool::pool_size(true, "mainx", 0x1E, 0x80000000),
           std::uint64_t{0x80000000});
  CHECK_EQ(pool::pool_size(true, "", 0x1E, 0x80000000),
           std::uint64_t{0x80000000});
  CHECK_EQ(pool::pool_size(true, "main", 0x1E, 0), std::uint64_t{0});
}

TEST_CASE("db pool: the page budget grows from 5 to 8 GiB, never past 16") {
  CHECK_EQ(pool::page_budget(0x140000000), std::uint64_t{0x200000000});
  CHECK_EQ(pool::page_budget(0x100000000), std::uint64_t{0x100000000});
  CHECK_EQ(pool::page_budget(0), std::uint64_t{0});
  CHECK_EQ(pool::page_budget(0x200000000), std::uint64_t{0x200000000});
  CHECK(pool::raised_page_budget <= pool::page_budget_ceiling);
  CHECK_EQ(pool::page_budget_ceiling, std::uint64_t{0x400000000});
  // 8 GiB of 64 KiB pages fits the 0x40000-page table.
  CHECK(pool::raised_page_budget / pool::page_size <= 0x40000);
}

TEST_CASE("db pool: names are copied without allocating, at most 15 bytes") {
  CHECK(pool::copy_pool_name(nullptr).view().empty());
  CHECK_EQ(pool::copy_pool_name("main").view(), std::string_view("main"));
  CHECK_EQ(pool::copy_pool_name("").view(), std::string_view(""));
  CHECK_EQ(pool::copy_pool_name("0123456789abcdefXYZ").view(),
           std::string_view("0123456789abcde"));
  const char exact[] = "0123456789abcde";
  CHECK_EQ(pool::copy_pool_name(exact).view(), std::string_view(exact));
}

TEST_CASE("db pool: the flag counts only before the first command") {
  CHECK(pool::requested({"-launch", "-xerces-db-pool", "+set", "a", "b"}));
  CHECK(pool::requested({"--xerces-db-pool"}));
  CHECK(!pool::requested({"-launch", "+set", "name", "-xerces-db-pool"}));
  CHECK(!pool::requested({"-launch", "-xerces-db-pools"}));
  CHECK(!pool::requested({}));
  CHECK(has_launch_flag({"-a", "-b"}, "-b"));
  CHECK(!has_launch_flag({"-a", "+b", "-b"}, "-b"));
}

TEST_CASE("db pool: the setting reaches the client's command line") {
  // Settings: the switch adds "-xerces-db-pool" to the extra flags and
  // reads any spelling back.
  std::vector<std::string> flags = {"-windowed"};
  flags = with_flag(flags, pool::flag_name, true);
  CHECK((flags == std::vector<std::string>{"-windowed", "-xerces-db-pool"}));
  CHECK(has_flag(flags, pool::flag_name));
  CHECK(has_flag({"--xerces-db-pool"}, pool::flag_name));
  CHECK(with_flag({"--xerces-db-pool", "-a"}, pool::flag_name, false) ==
        std::vector<std::string>{"-a"});
  CHECK(parse_launch_flags("-xerces-db-pool").reserved.empty());

  // The launch command keeps it before the mod dvars, where the client
  // looks for it.
  launch_options options;
  options.extra_flags = flags;
  options.dvars = {{"name", "-xerces-db-pool"}};
  const launch_command cmd = build_launch_command(nullptr, options);
  REQUIRE(cmd.ok());
  CHECK(pool::requested(cmd.args));
  options.extra_flags = {"-windowed"};
  CHECK(!pool::requested(build_launch_command(nullptr, options).args));
}

TEST_CASE("db pool: the report says what changed") {
  pool::report r = applied_report();
  CHECK(pool::pool_raised(r));
  CHECK(pool::budget_raised(r));
  CHECK(pool::applied(r));
  const std::string text = pool::message(r);
  CHECK(text.find("4096 MiB") != std::string::npos);
  CHECK(text.find("8192 MiB") != std::string::npos);
  CHECK(text.find("reads back 4096 MiB") != std::string::npos);
  const std::string data = pool::data_json(r);
  CHECK(data.find("\"applied\":true") != std::string::npos);
  CHECK(data.find("\"pool_calls\":23") != std::string::npos);
  CHECK(data.find("\"main_size_now\":\"0x100000000\"") != std::string::npos);
  CHECK(data.find("\"main_pages_now\":19010") != std::string::npos);
  CHECK(data.find("\"calls_not_recorded\":20") != std::string::npos);
  CHECK(data.find("\"bytes_given\":\"0x200000000\"") != std::string::npos);
  CHECK(json::parse(data).ok);
  // One call: its size, and the size given only when it differs.
  CHECK_EQ(pool::pool_call_json(r.calls[0]),
           std::string(R"({"flags":"0x1E","name":"main","reserved":true,)"
                       R"("size":"0x80000000","size_given":"0x100000000"})"));
  CHECK_EQ(pool::pool_call_json(r.calls[1]),
           std::string(R"({"flags":"0x3E","name":"gpu","reserved":true,)"
                       R"("size":"0x100000000"})"));

  // The hooks were installed but never called: the run proves nothing.
  pool::report none;
  CHECK(!pool::applied(none));
  CHECK(pool::message(none).find("never called") != std::string::npos);
  CHECK(json::parse(pool::data_json(none)).ok);

  // The first pool was something else: nothing raised, and it says what.
  pool::report other = applied_report();
  other.calls[0] = call("gpu", 0x3E, 0x100000000, 0x100000000);
  CHECK(!pool::pool_raised(other));
  CHECK(!pool::applied(other));
  CHECK(pool::message(other).find("\"gpu\"") != std::string::npos);

  // The reservation failed, or the size read back is not the raised one.
  pool::report failed = applied_report();
  failed.calls[0].result = 0;
  CHECK(!pool::applied(failed));
  CHECK(pool::message(failed).find("could not reserve") != std::string::npos);
  pool::report stale = applied_report();
  stale.main_size = 0x80000000;
  CHECK(!pool::applied(stale));
  CHECK(pool::message(stale).find("reads back 0x80000000") !=
        std::string::npos);

  // The page budget was not the stock one, or never set.
  pool::report budget = applied_report();
  budget.budget = pool::budget_call{0x100000000, 0x100000000, 0};
  CHECK(!pool::budget_raised(budget));
  CHECK(!pool::applied(budget));
  CHECK(pool::message(budget).find("page budget was 0x100000000") !=
        std::string::npos);
  budget.budget.reset();
  CHECK(pool::message(budget).find("page budget helper was never called") !=
        std::string::npos);
}

TEST_CASE("db pool: usage above the stock 2 GiB") {
  CHECK(!pool::above_stock(0));
  CHECK(!pool::above_stock(0x8000)); // exactly 2 GiB
  CHECK(pool::above_stock(0x8001));
  const std::string data = pool::usage_json(0x9900, 0xA000, "in_game");
  CHECK_EQ(data, std::string(R"({"above_stock":true,"mib":2448,"pages":39168,)"
                             R"("peak_mib":2560,"when":"in_game"})"));
  const std::string text = pool::usage_message(0x9900, 0xA000, "in_game");
  CHECK(text.find("In a level, the DB main pool holds 2448 MiB") !=
        std::string::npos);
  CHECK(text.find("more than the stock 2048 MiB") != std::string::npos);
  CHECK(text.find("at most 2560 MiB") != std::string::npos);
  CHECK(pool::usage_message(100, 100, "above_stock").find("within") !=
        std::string::npos);
}

TEST_CASE("entry code: the detoured bytes and the whole pattern must match") {
  const auto pattern = parse_pattern("?? ?? ?? ?? ?? 41 56 ?? 57");
  REQUIRE(pattern.has_value());
  const std::vector<std::uint8_t> code = {0x48, 0x8B, 0xC4, 0x41, 0x54,
                                          0x41, 0x56, 0x41, 0x57, 0x90};
  CHECK(entry_code_matches(code, pool::create_pool_entry, *pattern));
  // The jmp of a detour already there: the entry differs.
  auto hooked = code;
  hooked[0] = 0xE9;
  CHECK(!entry_code_matches(hooked, pool::create_pool_entry, *pattern));
  // A fixed byte of the pattern differs.
  auto other = code;
  other[6] = 0x57;
  CHECK(!entry_code_matches(other, pool::create_pool_entry, *pattern));
  // Not enough bytes to check either.
  CHECK(!entry_code_matches(std::span(code).first(8), pool::create_pool_entry,
                            *pattern));
  CHECK(!entry_code_matches(std::span(code).first(4), pool::create_pool_entry,
                            *parse_pattern("41")));
  CHECK(!entry_code_matches(code, {}, *pattern));
}

TEST_CASE("signature registry: the shipped file has both db pool helpers") {
  const auto file = xerces_tests::fixtures_dir().parent_path().parent_path() /
                    "compat" / "signatures.json";
  REQUIRE(std::filesystem::exists(file));
  const signature_registry registry =
      parse_signature_registry(xerces_tests::read_file(file));
  REQUIRE(registry.ok());
  const auto entry_of = [&](std::string_view id) -> const signature * {
    const auto it = std::ranges::find_if(
        registry.signatures, [&](const signature &s) { return s.id == id; });
    return it == registry.signatures.end() ? nullptr : &*it;
  };
  const std::vector<std::pair<std::string_view, std::vector<std::uint8_t>>>
      expected = {
          {db_pool_signatures::create_pool,
           {pool::create_pool_entry.begin(), pool::create_pool_entry.end()}},
          {db_pool_signatures::init_page_budget,
           {pool::init_page_budget_entry.begin(),
            pool::init_page_budget_entry.end()}},
  };
  for (const auto &[id, entry] : expected) {
    const signature *sig = entry_of(id);
    REQUIRE(sig != nullptr);
    CHECK(std::ranges::find(sig->features, pool::feature) !=
          sig->features.end());
    CHECK(sig->kind == symbol_kind::function);
    CHECK_EQ(sig->offset, std::int64_t{0});
    REQUIRE(!sig->patterns.empty());
    for (const std::string &text : sig->patterns) {
      const auto pattern = parse_pattern(text);
      REQUIRE(pattern.has_value());
      // The first five bytes are wildcards (a dump of a client with the
      // detour has a jmp there); the client checks them against `entry`,
      // and where the pattern does fix a byte the two agree.
      REQUIRE(pattern->bytes.size() > entry.size());
      for (std::size_t i = 0; i < 5; ++i) {
        CHECK(!pattern->fixed[i]);
      }
      for (std::size_t i = 5; i < entry.size(); ++i) {
        CHECK(pattern->fixed[i]);
        CHECK_EQ(pattern->bytes[i], entry[i]);
      }
    }
  }
  CHECK_EQ(entry_of(db_pool_signatures::create_pool)->known.at(0x6531394),
           std::uint64_t{0x20C3520});
  CHECK_EQ(entry_of(db_pool_signatures::init_page_budget)->known.at(0x6531394),
           std::uint64_t{0x20C3F90});
  // Nothing else is tagged db_pool.
  for (const auto &sig : registry.signatures) {
    if (std::ranges::find(sig.features, pool::feature) != sig.features.end()) {
      CHECK(std::ranges::find(db_pool_signatures::all, sig.id) !=
            db_pool_signatures::all.end());
    }
  }
}
