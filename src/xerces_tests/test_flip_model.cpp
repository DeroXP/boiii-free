// Flip-model presentation (docs/DESIGN.md 12.4 item 4): what the client's
// detours decide for the engine's swap chain, ResizeBuffers and Present, what
// they log, and the launch flag from the launcher's setting to the client's
// check.

#include "harness.hpp"

#include <string>
#include <vector>

#include <xerces_core/flip_model.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/perf_bench.hpp>
#include <xerces_core/settings_options.hpp>

using namespace xerces_core;
namespace flip = xerces_core::flip_model;
namespace dx = xerces_core::flip_model::dxgi;

namespace {
// The engine's description as perf.swapchain logged it on 2026-11-05 and the
// dump's desc builder fills it: 3840x2160, R8G8B8A8_UNORM, no MSAA, render
// target output and shader input, 2 buffers, windowed, DISCARD,
// ALLOW_MODE_SWITCH.
flip::chain_desc engine_desc() {
  flip::chain_desc d;
  d.width = 3840;
  d.height = 2160;
  d.format = dx::format_r8g8b8a8_unorm;
  d.sample_count = 1;
  d.sample_quality = 0;
  d.buffer_usage = dx::usage_render_target_output | dx::usage_shader_input;
  d.buffer_count = 2;
  d.windowed = true;
  d.swap_effect = dx::effect_discard;
  d.flags = dx::chain_allow_mode_switch;
  return d;
}

bool parses(const std::string &text) { return json::parse(text).ok; }

bool contains(const std::string &text, std::string_view part) {
  return text.find(part) != std::string::npos;
}
} // namespace

TEST_CASE("flip model: the engine's chain becomes FLIP_DISCARD") {
  const flip::chain_desc engine = engine_desc();
  const flip::decision d = flip::decide(engine, true);
  CHECK(d.applied());
  CHECK(d.tearing);
  CHECK_EQ(d.desc.swap_effect, dx::effect_flip_discard);
  // ALLOW_MODE_SWITCH stays, ALLOW_TEARING is added.
  CHECK_EQ(d.desc.flags, dx::chain_allow_mode_switch | dx::chain_allow_tearing);
  // Everything else is the engine's.
  flip::chain_desc rest = d.desc;
  rest.swap_effect = engine.swap_effect;
  rest.flags = engine.flags;
  CHECK(rest == engine);

  const flip::decision no_tearing = flip::decide(engine, false);
  CHECK(no_tearing.applied());
  CHECK(!no_tearing.tearing);
  CHECK_EQ(no_tearing.desc.swap_effect, dx::effect_flip_discard);
  CHECK_EQ(no_tearing.desc.flags, dx::chain_allow_mode_switch);
}

TEST_CASE("flip model: every format flip model takes, and 2 to 16 buffers") {
  for (const std::uint32_t format :
       {dx::format_r8g8b8a8_unorm, dx::format_b8g8r8a8_unorm,
        dx::format_r16g16b16a16_float, dx::format_r10g10b10a2_unorm}) {
    flip::chain_desc engine = engine_desc();
    engine.format = format;
    CHECK(flip::decide(engine, true).applied());
  }
  for (const std::uint32_t count : {2u, 3u, 16u}) {
    flip::chain_desc engine = engine_desc();
    engine.buffer_count = count;
    CHECK(flip::decide(engine, true).applied());
  }
  flip::chain_desc engine = engine_desc();
  engine.windowed = false; // made fullscreen at once: still fine
  CHECK(flip::decide(engine, true).applied());
  engine.flags = 0;
  CHECK_EQ(flip::decide(engine, true).desc.flags, dx::chain_allow_tearing);
}

TEST_CASE("flip model: chains it cannot take are left as the engine asked") {
  const auto check_kept = [](const flip::chain_desc &engine,
                             flip::outcome why) {
    const flip::decision d = flip::decide(engine, true);
    CHECK(!d.applied());
    CHECK(!d.tearing);
    CHECK_EQ(flip::to_string(d.result), flip::to_string(why));
    CHECK(d.desc == engine); // nothing changed, not even the flags
  };
  flip::chain_desc e = engine_desc();
  e.swap_effect = dx::effect_flip_discard;
  check_kept(e, flip::outcome::already_flip);
  e.swap_effect = dx::effect_flip_sequential;
  check_kept(e, flip::outcome::already_flip);
  e.swap_effect = dx::effect_sequential;
  check_kept(e, flip::outcome::not_discard);
  e.swap_effect = 2; // no such swap effect
  check_kept(e, flip::outcome::not_discard);

  for (const std::uint32_t count : {0u, 1u, 17u}) {
    e = engine_desc();
    e.buffer_count = count;
    check_kept(e, flip::outcome::buffer_count);
  }

  e = engine_desc();
  e.sample_count = 4;
  check_kept(e, flip::outcome::multisampled);
  e = engine_desc();
  e.sample_quality = 1;
  check_kept(e, flip::outcome::multisampled);

  // sRGB is skipped, never converted to the plain format.
  for (const std::uint32_t format :
       {dx::format_r8g8b8a8_unorm_srgb, dx::format_b8g8r8a8_unorm_srgb}) {
    e = engine_desc();
    e.format = format;
    check_kept(e, flip::outcome::srgb_format);
  }
  for (const std::uint32_t format : {0u, 2u, 85u, 61u}) {
    e = engine_desc();
    e.format = format; // UNKNOWN, R32G32B32A32_FLOAT, B5G6R5, R8_UNORM
    check_kept(e, flip::outcome::format);
  }

  for (const std::uint32_t usage : {0x80u, 0x100u, 0x200u, 0x1u}) {
    e = engine_desc();
    e.buffer_usage |= usage; // SHARED, READ_ONLY, DISCARD_ON_PRESENT, CPU
    check_kept(e, flip::outcome::usage);
  }

  for (const std::uint32_t flag : {0x1u, 0x4u, 0x20u, 0x200u}) {
    e = engine_desc();
    e.flags |= flag; // NONPREROTATED, GDI_COMPATIBLE, DISPLAY_ONLY, YUV_VIDEO
    check_kept(e, flip::outcome::flags);
  }
}

TEST_CASE("flip model: ResizeBuffers keeps the flags fixed at creation") {
  const std::uint32_t created =
      dx::chain_allow_mode_switch | dx::chain_allow_tearing;
  // The engine's own call: its buffer count, format and flags 0x2.
  const flip::resize_request engine{2, dx::format_r8g8b8a8_unorm,
                                    dx::chain_allow_mode_switch};
  const flip::resize_request given = flip::resize(engine, created);
  CHECK_EQ(given.buffer_count, 2u);
  CHECK_EQ(given.format, dx::format_r8g8b8a8_unorm);
  CHECK_EQ(given.flags, created);

  // 0 keeps the count and the format; 1 is too few for flip model.
  CHECK_EQ(flip::resize({0, 0, 0}, created).buffer_count, 0u);
  CHECK_EQ(flip::resize({0, 0, 0}, created).format, 0u);
  CHECK_EQ(flip::resize({0, 0, 0}, created).flags, dx::chain_allow_tearing);
  CHECK_EQ(flip::resize({1, 0, 0}, created).buffer_count, 2u);
  CHECK_EQ(flip::resize({3, 0, 0}, created).buffer_count, 3u);

  // A chain made without tearing never gets it later, and the waitable flag
  // comes from the creation too.
  CHECK_EQ(flip::resize({2, 0, dx::chain_allow_tearing}, 0).flags, 0u);
  CHECK_EQ(flip::resize({2, 0, 0}, dx::chain_frame_latency_waitable).flags,
           dx::chain_frame_latency_waitable);
  // Other flags the engine passes stay as it passed them.
  CHECK_EQ(flip::resize({2, 0, 0}, created).flags, dx::chain_allow_tearing);
}

TEST_CASE("flip model: Present gets the tearing flag only where DXGI allows") {
  CHECK_EQ(flip::present_flags(0, 0, true, false), dx::present_allow_tearing);
  CHECK_EQ(flip::present_flags(0x4, 0, true, false),
           0x4u | dx::present_allow_tearing);
  CHECK_EQ(flip::present_flags(0, 1, true, false), 0u); // V-Sync on
  CHECK_EQ(flip::present_flags(0, 2, true, false), 0u);
  CHECK_EQ(flip::present_flags(0, 0, true, true), 0u);   // exclusive fullscreen
  CHECK_EQ(flip::present_flags(0, 0, false, false), 0u); // made without it
  CHECK_EQ(flip::present_flags(dx::present_test, 0, true, false),
           dx::present_test);
}

TEST_CASE("flip model: the launch flag, from the setting to the client") {
  CHECK(flip::requested({"-launch", "-xerces-flip-model"}));
  CHECK(flip::requested({"--xerces-flip-model"}));
  CHECK(!flip::requested({"-launch"}));
  CHECK(!flip::requested({"-xerces-flip-modelx"}));
  // Flags only count before the first "+" command.
  CHECK(!flip::requested({"+set", "x", "-xerces-flip-model"}));

  // The launcher's toggle writes exactly what the client looks for.
  std::vector<std::string> flags = {"-windowed"};
  flags = with_flag(flags, flip::flag_name, true);
  CHECK((flags == std::vector<std::string>{"-windowed", "-xerces-flip-model"}));
  CHECK(has_flag(flags, flip::flag_name));
  CHECK(flip::requested(flags));
  CHECK(has_flag({"--xerces-flip-model"}, flip::flag_name));
  flags = with_flag(flags, flip::flag_name, false);
  CHECK((flags == std::vector<std::string>{"-windowed"}));
  CHECK(!flip::requested(flags));
  CHECK_EQ(std::string(flip::flag), "-" + std::string(flip::flag_name));
}

TEST_CASE("flip model: xerces.flip_model says what was asked and done") {
  flip::report r;
  r.chain = 1;
  r.asked = engine_desc();
  const flip::decision d = flip::decide(*r.asked, true);
  r.result = d.result;
  r.made = d.desc;
  r.tearing_supported = true;
  r.tearing = d.tearing;
  const std::string message = flip::message(r);
  CHECK(contains(message, "FLIP_DISCARD"));
  CHECK(contains(message, "the engine asked for DISCARD"));
  CHECK(contains(message, "tearing allowed"));
  const std::string text = flip::data_json(r);
  REQUIRE(parses(text));
  const auto parsed = json::parse(text);
  const auto &doc = parsed.document;
  CHECK(doc["requested"].GetBool());
  CHECK(doc["applied"].GetBool());
  CHECK_EQ(std::string(doc["reason"].GetString()), std::string("applied"));
  CHECK_EQ(doc["asked"]["swap_effect"].GetInt64(), std::int64_t{0});
  CHECK_EQ(std::string(doc["asked"]["swap_effect_name"].GetString()),
           std::string("discard"));
  CHECK_EQ(doc["made"]["swap_effect"].GetInt64(), std::int64_t{4});
  CHECK_EQ(std::string(doc["made"]["flags"].GetString()), std::string("0x802"));
  CHECK_EQ(std::string(doc["asked"]["usage"].GetString()), std::string("0x30"));
  CHECK(doc["tearing"].GetBool());
  CHECK(!doc.HasMember("flip_hr"));

  // Kept as asked, with the reason.
  flip::report srgb;
  srgb.chain = 1;
  srgb.asked = engine_desc();
  srgb.asked->format = dx::format_r8g8b8a8_unorm_srgb;
  srgb.result = flip::decide(*srgb.asked, true).result;
  srgb.made = srgb.asked;
  CHECK(contains(flip::message(srgb), "not used"));
  CHECK(contains(flip::message(srgb), "sRGB"));
  CHECK(contains(flip::message(srgb), "presents as before (DISCARD)"));
  REQUIRE(parses(flip::data_json(srgb)));
  CHECK(contains(flip::data_json(srgb), "\"reason\":\"srgb_format\""));
  CHECK(contains(flip::data_json(srgb), "\"applied\":false"));

  // DXGI refused the flip chain: its answer is in the line.
  flip::report refused;
  refused.chain = 2;
  refused.asked = engine_desc();
  refused.result = flip::outcome::create_failed;
  refused.flip_hr = 0x887A0001;
  refused.made = refused.asked;
  CHECK(contains(flip::message(refused), "0x887A0001"));
  CHECK(contains(flip::data_json(refused), "\"flip_hr\":\"0x887A0001\""));
  CHECK(contains(flip::data_json(refused), "\"chain\":2"));

  // A detour that failed, and a chain never seen: no description at all.
  flip::report failed;
  failed.result = flip::outcome::hook_failed;
  failed.error = "CreateSwapChain: this function has already been hooked.";
  CHECK(contains(flip::message(failed), "already been hooked"));
  REQUIRE(parses(flip::data_json(failed)));
  CHECK(!contains(flip::data_json(failed), "\"asked\""));
  flip::report unseen;
  CHECK_EQ(flip::to_string(unseen.result), std::string_view("not_seen"));
  CHECK(contains(flip::message(unseen), "presents as before"));

  // The engine's own call failed too.
  flip::report both = refused;
  both.made.reset();
  both.create_hr = 0x887A0001;
  CHECK(contains(flip::message(both), "failed too"));
}

TEST_CASE("flip model: every outcome has its own name and reason") {
  const flip::outcome all[] = {
      flip::outcome::not_requested,   flip::outcome::applied,
      flip::outcome::already_flip,    flip::outcome::not_discard,
      flip::outcome::buffer_count,    flip::outcome::multisampled,
      flip::outcome::srgb_format,     flip::outcome::format,
      flip::outcome::usage,           flip::outcome::flags,
      flip::outcome::hooks_not_ready, flip::outcome::hook_failed,
      flip::outcome::create_failed,   flip::outcome::not_seen,
  };
  std::vector<std::string> names;
  for (const flip::outcome value : all) {
    const std::string name(flip::to_string(value));
    CHECK(name != "unknown");
    CHECK(std::string(flip::explain(value)) != "unknown");
    for (const std::string &seen : names) {
      CHECK(seen != name);
    }
    names.push_back(name);
  }
  // perf.swapchain's "off" is the not-requested outcome's name.
  CHECK_EQ(flip::to_string(flip::outcome::not_requested),
           std::string_view("off"));
}

TEST_CASE("flip model: the ResizeBuffers failure line") {
  const std::string text = flip::resize_failure_json(
      {2, 28, 0x2}, {2, 28, 0x802}, 3840, 2160, 0x887A0001);
  REQUIRE(parses(text));
  CHECK(contains(text, "\"hr\":\"0x887A0001\""));
  // Canonical JSON: keys in byte order.
  CHECK(contains(text, "\"given\":{\"buffer_count\":2,\"flags\":\"0x802\","
                       "\"format\":28}"));
  CHECK(contains(text, "\"width\":3840"));
}

TEST_CASE("flip model: perf.swapchain shows the effective swap effect") {
  bench::swapchain_info info;
  info.has_desc = true;
  info.swap_effect = dx::effect_flip_discard; // what GetDesc reports
  info.flags = 0x802;
  CHECK(contains(bench::swapchain_json(info), "\"flip_model\":\"off\""));
  CHECK(contains(bench::swapchain_json(info), "\"engine_swap_effect\":null"));
  info.flip_model = std::string(flip::to_string(flip::outcome::applied));
  info.engine_swap_effect = dx::effect_discard;
  info.engine_flags = dx::chain_allow_mode_switch;
  const std::string text = bench::swapchain_json(info);
  REQUIRE(parses(text));
  CHECK(contains(text, "\"swap_effect_name\":\"flip_discard\""));
  CHECK(contains(text, "\"flip_model\":\"applied\""));
  CHECK(contains(text, "\"engine_swap_effect\":0"));
  CHECK(contains(text, "\"engine_swap_effect_name\":\"discard\""));
  CHECK(contains(text, "\"engine_flags\":\"0x2\""));
}

TEST_CASE("flip model: arming works in either start-up order") {
  // The usual order in the game: the overlay's Present hook (async thread)
  // ends before this component's post_unpack. post_unpack then installs
  // ResizeBuffers itself.
  flip::arming usual;
  CHECK(!flip::arm_present_hook(usual)); // the flag is not known yet
  CHECK(flip::not_armed(usual) == flip::outcome::hooks_not_ready);
  CHECK(flip::arm_create_hook(usual, true));
  CHECK(usual.armed_by == flip::arm_step::create_hook);
  CHECK(flip::not_armed(usual) == flip::outcome::hooks_not_ready);
  CHECK_EQ(flip::missing(usual),
           std::string("the ResizeBuffers detour is not in place yet"));
  flip::arm_resize_hook(usual, true);
  CHECK(!flip::not_armed(usual));
  CHECK(flip::missing(usual).empty());
  CHECK_EQ(flip::to_string(*usual.armed_by), std::string_view("post_unpack"));

  // The other order: post_unpack first, the Present hook installs it.
  flip::arming late;
  CHECK(!flip::arm_create_hook(late, true));
  CHECK(flip::not_armed(late) == flip::outcome::hooks_not_ready);
  CHECK_EQ(flip::missing(late),
           std::string("the overlay's Present detour is not in place"));
  CHECK(flip::arm_present_hook(late));
  CHECK(late.armed_by == flip::arm_step::present_hook);
  flip::arm_resize_hook(late, true);
  CHECK(!flip::not_armed(late));
  CHECK_EQ(flip::to_string(*late.armed_by), std::string_view("present_hook"));

  // Handed out once only, whatever comes again.
  CHECK(!flip::arm_present_hook(late));
  CHECK(!flip::arm_create_hook(late, true));
}

TEST_CASE("flip model: arming never flips without every detour") {
  // Without the flag post_unpack never arms: the Present hook alone does
  // nothing.
  flip::arming off;
  CHECK(!flip::arm_present_hook(off));
  CHECK(!off.resize_due);
  CHECK(flip::not_armed(off) == flip::outcome::hooks_not_ready);

  // The overlay's setup failed (no Present hook): never armed, and the
  // report says what is missing.
  flip::arming no_present;
  CHECK(!flip::arm_create_hook(no_present, true));
  CHECK(flip::not_armed(no_present) == flip::outcome::hooks_not_ready);
  CHECK(contains(flip::missing(no_present), "Present"));

  // CreateSwapChain could not be detoured: the Present hook does not arm,
  // and the failure is kept.
  flip::arming no_create;
  CHECK(!flip::arm_create_hook(no_create, false,
                               "CreateSwapChain: already been hooked"));
  CHECK(!flip::arm_present_hook(no_create));
  CHECK(flip::not_armed(no_create) == flip::outcome::hook_failed);
  CHECK_EQ(flip::missing(no_create),
           std::string("CreateSwapChain: already been hooked"));

  // ResizeBuffers failed: hook_failed stays, never hooks_not_ready.
  flip::arming no_resize;
  CHECK(!flip::arm_present_hook(no_resize));
  CHECK(flip::arm_create_hook(no_resize, true));
  flip::arm_resize_hook(no_resize, false,
                        "ResizeBuffers: MH_ERROR_MEMORY_ALLOC");
  CHECK(flip::not_armed(no_resize) == flip::outcome::hook_failed);
  CHECK(!no_resize.resize_hooked);
  CHECK_EQ(flip::missing(no_resize),
           std::string("ResizeBuffers: MH_ERROR_MEMORY_ALLOC"));
  // A later step does not hand it out again or hide the failure.
  CHECK(!flip::arm_present_hook(no_resize));
  CHECK(flip::not_armed(no_resize) == flip::outcome::hook_failed);

  // An install that was never handed out is ignored.
  flip::arming stray;
  flip::arm_resize_hook(stray, true);
  CHECK(!stray.resize_hooked);
  CHECK(flip::not_armed(stray) == flip::outcome::hooks_not_ready);

  // The report's line names the missing detour.
  flip::report r;
  r.chain = 1;
  r.asked = engine_desc();
  r.made = r.asked;
  r.result = *flip::not_armed(no_present);
  r.error = flip::missing(no_present);
  CHECK(contains(flip::message(r), "not both in place"));
  CHECK(contains(flip::message(r), "Present detour is not in place"));
  CHECK(contains(flip::data_json(r), "\"reason\":\"hooks_not_ready\""));
}
