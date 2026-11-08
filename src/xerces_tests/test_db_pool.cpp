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

// What build 0x6531394 does: the DB main pool first, then the rest, the DB
// stack's streamed-texture pool third.
pool::report applied_report() {
  pool::report r;
  r.calls = {call("main", 0x1E, 0x80000000, 0x100000000),
             call("gpu", 0x3E, 0x100000000, 0x100000000),
             call("gpu_prt", 0x7E, 0x480000000, 0x780000000),
             call("main", 0x1E, 0x80000000, 0x80000000),
             call("gpu_prt", 0x7E, 0x480000000, 0x480000000)};
  r.pool_calls = 23;
  r.budget = pool::budget_call{0x140000000, 0x200000000, 0};
  r.budget_calls = 1;
  r.main_size = 0x100000000;
  r.main_pages = 19010;
  return r;
}
} // namespace

TEST_CASE("db pool: only the first call, the stock DB main pool, grows") {
  CHECK_EQ(pool::pool_size(true, false, "main", 0x1E, 0x80000000),
           std::uint64_t{0x100000000});
  // Five more "main" pools of 2 GiB with the same flags come later (other
  // stacks): they keep their size.
  CHECK_EQ(pool::pool_size(false, false, "main", 0x1E, 0x80000000),
           std::uint64_t{0x80000000});
  // A first call that is anything else is left alone.
  CHECK_EQ(pool::pool_size(true, false, "gpu", 0x1E, 0x80000000),
           std::uint64_t{0x80000000});
  CHECK_EQ(pool::pool_size(true, false, "main", 0x3E, 0x80000000),
           std::uint64_t{0x80000000});
  CHECK_EQ(pool::pool_size(true, false, "main", 0x1E, 0x10000000),
           std::uint64_t{0x10000000});
  CHECK_EQ(pool::pool_size(true, false, "mainx", 0x1E, 0x80000000),
           std::uint64_t{0x80000000});
  CHECK_EQ(pool::pool_size(true, false, "", 0x1E, 0x80000000),
           std::uint64_t{0x80000000});
  CHECK_EQ(pool::pool_size(true, false, "main", 0x1E, 0), std::uint64_t{0});
}

TEST_CASE("db pool: only the first stock gpu_prt pool grows, below 32 GiB") {
  CHECK_EQ(pool::pool_size(false, true, "gpu_prt", 0x7E, 0x480000000),
           std::uint64_t{0x780000000});
  // The three later "gpu_prt" pools (other stacks) keep their size.
  CHECK_EQ(pool::pool_size(false, false, "gpu_prt", 0x7E, 0x480000000),
           std::uint64_t{0x480000000});
  // Anything but the stock one is left alone.
  CHECK_EQ(pool::pool_size(false, true, "gpu_prt", 0x3E, 0x480000000),
           std::uint64_t{0x480000000});
  CHECK_EQ(pool::pool_size(false, true, "gpu_prt", 0x7E, 0x80000000),
           std::uint64_t{0x80000000});
  CHECK_EQ(pool::pool_size(false, true, "gpu", 0x7E, 0x480000000),
           std::uint64_t{0x480000000});
  // A pool's page index is 19 bits: the raised size stays below 32 GiB.
  CHECK(pool::raised_prt_size < pool::pool_size_ceiling);
  CHECK(pool::raised_prt_size / pool::page_size < 0x7FFFF);
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
  CHECK(pool::prt_raised(r));
  CHECK(pool::budget_raised(r));
  CHECK(pool::applied(r));
  const std::string text = pool::message(r);
  CHECK(text.find("4096 MiB") != std::string::npos);
  CHECK(text.find("streamed-texture pool 30720 MiB (stock 18432 MiB)") !=
        std::string::npos);
  CHECK(text.find("8192 MiB") != std::string::npos);
  CHECK(text.find("reads back 4096 MiB") != std::string::npos);
  const std::string data = pool::data_json(r);
  CHECK(data.find("\"applied\":true") != std::string::npos);
  CHECK(data.find("\"pool_calls\":23") != std::string::npos);
  CHECK(data.find("\"main_size_now\":\"0x100000000\"") != std::string::npos);
  CHECK(data.find("\"main_pages_now\":19010") != std::string::npos);
  CHECK(data.find("\"calls_not_recorded\":18") != std::string::npos);
  CHECK(data.find("\"prt_raised\":true") != std::string::npos);
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

  // The streamed-texture pool was not the stock one, or its reservation
  // failed, or none was created: the rest still applies, the report says so.
  pool::report prt = applied_report();
  prt.calls[2] = call("gpu_prt", 0x7E, 0x80000000, 0x80000000);
  CHECK(!pool::prt_raised(prt));
  CHECK(!pool::applied(prt));
  CHECK(pool::message(prt).find("first \"gpu_prt\" pool was flags 0x7E "
                                "size 0x80000000") != std::string::npos);
  prt.calls[2] = call("gpu_prt", 0x7E, 0x480000000, 0x780000000, 0);
  CHECK(pool::message(prt).find("could not reserve the raised "
                                "streamed-texture pool") != std::string::npos);
  prt.calls.erase(prt.calls.begin() + 2);
  prt.calls.pop_back();
  CHECK(pool::message(prt).find("no \"gpu_prt\" pool was recorded") !=
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
  // Each phase the journal enters (whether the pool empties between maps).
  CHECK(pool::usage_message(0x9900, 0x9900, "frontend")
            .starts_with("In the front end, the DB main pool holds 2448 MiB"));
  CHECK(pool::usage_message(0x9900, 0x9900, "loading_map")
            .starts_with("As a level starts loading, the DB main pool holds"));
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

TEST_CASE("db pool: the size is 4, 6 or 8 GiB and the budget follows it") {
  CHECK_EQ(pool::pool_bytes_for(4), std::uint64_t{0x100000000});
  CHECK_EQ(pool::pool_bytes_for(6), std::uint64_t{0x180000000});
  CHECK_EQ(pool::pool_bytes_for(8), std::uint64_t{0x200000000});
  // 4 GiB keeps the first build's 8 GiB budget; the others grow with the
  // pool and stay inside the 16 GiB the budget's page table holds.
  CHECK_EQ(pool::page_budget_for(4), pool::raised_page_budget);
  CHECK_EQ(pool::page_budget_for(6), std::uint64_t{0x280000000}); // 10 GiB
  CHECK_EQ(pool::page_budget_for(8), std::uint64_t{0x300000000}); // 12 GiB
  for (const int gib : pool::size_options_gib) {
    CHECK(pool::page_budget_for(gib) <= pool::page_budget_ceiling);
    CHECK(pool::page_budget_for(gib) / pool::page_size <= 0x40000);
    CHECK(pool::pool_bytes_for(gib) < pool::pool_size_ceiling);
    // The budget always covers the raised pool and the 2 GiB the stock one
    // left to everything else.
    CHECK(pool::page_budget_for(gib) >=
          pool::pool_bytes_for(gib) + pool::stock_pool_size);
  }
}

TEST_CASE("db pool: the pool and budget the detours give follow the target") {
  const std::uint64_t six = pool::pool_bytes_for(6);
  CHECK_EQ(pool::pool_size(true, false, "main", 0x1E, 0x80000000, six), six);
  // Only the stock DB main pool is raised, whatever the target.
  CHECK_EQ(pool::pool_size(false, false, "main", 0x1E, 0x80000000, six),
           std::uint64_t{0x80000000});
  CHECK_EQ(pool::pool_size(true, false, "main", 0x1E, 0x10000000, six),
           std::uint64_t{0x10000000});
  // The streamed-texture pool is 30 GiB whatever the main pool is.
  CHECK_EQ(pool::pool_size(false, true, "gpu_prt", 0x7E, 0x480000000, six),
           pool::raised_prt_size);
  CHECK_EQ(pool::page_budget(0x140000000, pool::page_budget_for(6)),
           std::uint64_t{0x280000000});
  CHECK_EQ(pool::page_budget(0x100000000, pool::page_budget_for(6)),
           std::uint64_t{0x100000000});
  // The defaults are the first build's 4 GiB and 8 GiB.
  CHECK_EQ(pool::pool_size(true, false, "main", 0x1E, 0x80000000),
           pool::raised_pool_size);
  CHECK_EQ(pool::page_budget(0x140000000), pool::raised_page_budget);
}

TEST_CASE("db pool: the size flags pick 4, 6 or 8 GiB, only with the flag") {
  CHECK_EQ(pool::requested_gib({}), 0);
  CHECK_EQ(pool::requested_gib({"-windowed"}), 0);
  CHECK_EQ(pool::requested_gib({"-xerces-db-pool"}), 4);
  CHECK_EQ(pool::requested_gib({"-xerces-db-pool", "-xerces-db-pool-6gb"}), 6);
  CHECK_EQ(pool::requested_gib({"-xerces-db-pool-8gb", "--xerces-db-pool"}), 8);
  CHECK_EQ(pool::requested_gib({"--xerces-db-pool", "--xerces-db-pool-4gb"}),
           4);
  // A size alone turns nothing on.
  CHECK_EQ(pool::requested_gib({"-xerces-db-pool-8gb"}), 0);
  // The last size wins when both are given by hand.
  CHECK_EQ(pool::requested_gib({"-xerces-db-pool", "-xerces-db-pool-6gb",
                                "-xerces-db-pool-8gb"}),
           8);
  CHECK_EQ(pool::requested_gib({"-xerces-db-pool", "-xerces-db-pool-8gb",
                                "-xerces-db-pool-6gb"}),
           6);
  // Anything else is 4 GiB: other sizes, other spellings, flags that only
  // start the same.
  for (const char *other :
       {"-xerces-db-pool-5gb", "-xerces-db-pool-2gb", "-xerces-db-pool-16gb",
        "-xerces-db-pool-6", "-xerces-db-pool-6g", "-xerces-db-pool-6gbx",
        "-xerces-db-pool-gb", "-xerces-db-pool-gb=6", "xerces-db-pool-6gb",
        "-xerces-db-pools"}) {
    CHECK_EQ(pool::requested_gib({"-xerces-db-pool", other}), 4);
  }
  // Flags count only before the first command, like the flag itself.
  CHECK_EQ(pool::requested_gib(
               {"-xerces-db-pool", "+set", "a", "b", "-xerces-db-pool-8gb"}),
           4);
  CHECK_EQ(pool::requested_gib(
               {"+set", "a", "b", "-xerces-db-pool", "-xerces-db-pool-8gb"}),
           0);
}

TEST_CASE("db pool: the settings' choice reaches the command line") {
  using flags = std::vector<std::string>;
  CHECK((pool::with_size({"-windowed"}, 0) == flags{"-windowed"}));
  CHECK((pool::with_size({"-windowed"}, 4) ==
         flags{"-windowed", "-xerces-db-pool"}));
  CHECK((pool::with_size({"-windowed"}, 6) ==
         flags{"-windowed", "-xerces-db-pool", "-xerces-db-pool-6gb"}));
  CHECK((pool::with_size({"-windowed"}, 8) ==
         flags{"-windowed", "-xerces-db-pool", "-xerces-db-pool-8gb"}));
  // A change replaces the old choice, whichever way it was spelled, and
  // keeps every other flag in its place.
  CHECK((pool::with_size(
             {"-a", "--xerces-db-pool", "-b", "--xerces-db-pool-6gb"}, 8) ==
         flags{"-a", "-b", "-xerces-db-pool", "-xerces-db-pool-8gb"}));
  CHECK((pool::with_size({"-a", "-xerces-db-pool", "-xerces-db-pool-8gb"}, 4) ==
         flags{"-a", "-xerces-db-pool"}));
  CHECK((pool::with_size({"-a", "-xerces-db-pool", "-xerces-db-pool-8gb"}, 0) ==
         flags{"-a"}));
  // What the settings write, the client and the planner read back.
  for (const int gib : {0, 4, 6, 8}) {
    CHECK_EQ(pool::requested_gib(pool::with_size({"-x"}, gib)), gib);
  }
  // A size flag is a launch flag like the others: the settings' flag field
  // takes it, and it passes the launch command.
  const flags_parse typed =
      parse_launch_flags("-xerces-db-pool -xerces-db-pool-8gb");
  CHECK(typed.invalid.empty());
  CHECK(typed.reserved.empty());
  CHECK_EQ(pool::requested_gib(typed.flags), 8);
  launch_options options;
  options.extra_flags = pool::with_size({"-windowed"}, 6);
  const launch_command cmd = build_launch_command(nullptr, options);
  REQUIRE(cmd.ok());
  CHECK_EQ(pool::requested_gib(cmd.args), 6);
  // Turning the switch off the old way (the culprit search) leaves a size
  // that does nothing alone.
  CHECK_EQ(
      pool::requested_gib(with_flag({"-xerces-db-pool", "-xerces-db-pool-8gb"},
                                    pool::flag_name, false)),
      0);
}

TEST_CASE("db pool: the report reads against the size the launch asked for") {
  for (const int gib : {6, 8}) {
    pool::report r = applied_report();
    r.target_pool = pool::pool_bytes_for(gib);
    r.target_budget = pool::page_budget_for(gib);
    r.calls[0].size_out = r.target_pool;
    r.budget->bytes_out = r.target_budget;
    r.main_size = r.target_pool;
    CHECK(pool::pool_raised(r));
    CHECK(pool::budget_raised(r));
    CHECK(pool::applied(r));
    const std::string text = pool::message(r);
    CHECK(text.find("the DB main pool is " + std::to_string(gib * 1024) +
                    " MiB") != std::string::npos);
    CHECK(text.find("the page budget " + std::to_string((gib + 4) * 1024) +
                    " MiB") != std::string::npos);
    CHECK(text.find("reads back " + std::to_string(gib * 1024) + " MiB") !=
          std::string::npos);
    CHECK(pool::data_json(r).find("\"pool_gib\":" + std::to_string(gib)) !=
          std::string::npos);
    CHECK(json::parse(pool::data_json(r)).ok);
    // A 4 GiB pool in a bigger launch is not what was asked for.
    pool::report short_pool = r;
    short_pool.calls[0].size_out = pool::raised_pool_size;
    CHECK(!pool::applied(short_pool));
    pool::report short_budget = r;
    short_budget.budget->bytes_out = pool::raised_page_budget;
    CHECK(!pool::applied(short_budget));
  }
  // The default report is still the first build's 4 GiB.
  CHECK_EQ(pool::report{}.target_pool, pool::raised_pool_size);
  CHECK_EQ(pool::report{}.target_budget, pool::raised_page_budget);
  CHECK(pool::data_json(applied_report()).find("\"pool_gib\":4") !=
        std::string::npos);
}
