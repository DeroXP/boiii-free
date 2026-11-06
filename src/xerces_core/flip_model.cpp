#include "flip_model.hpp"

#include <cstdio>

#include "json.hpp"
#include "launch.hpp"
#include "perf_bench.hpp"

namespace xerces_core::flip_model {
namespace {
std::string hex(std::uint32_t value) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%X", value);
  return text;
}

bool srgb(std::uint32_t format) {
  return format == dxgi::format_r8g8b8a8_unorm_srgb ||
         format == dxgi::format_b8g8r8a8_unorm_srgb;
}

bool flip_format(std::uint32_t format) {
  return format == dxgi::format_r16g16b16a16_float ||
         format == dxgi::format_r10g10b10a2_unorm ||
         format == dxgi::format_r8g8b8a8_unorm ||
         format == dxgi::format_b8g8r8a8_unorm;
}

// Usage bits a flip-model chain's buffers take; the engine asks for render
// target output and shader input (0x30).
constexpr std::uint32_t flip_usage =
    dxgi::usage_shader_input | dxgi::usage_render_target_output |
    dxgi::usage_back_buffer | dxgi::usage_unordered_access;

// Chain flags known to work with flip model; the engine asks for
// ALLOW_MODE_SWITCH only. Others (GDI_COMPATIBLE, DISPLAY_ONLY, the video and
// protected-content flags) leave the chain alone.
constexpr std::uint32_t flip_flags = dxgi::chain_allow_mode_switch |
                                     dxgi::chain_frame_latency_waitable |
                                     dxgi::chain_allow_tearing;

// Fixed when the chain is created: ResizeBuffers may not add or drop them.
constexpr std::uint32_t creation_flags =
    dxgi::chain_allow_tearing | dxgi::chain_frame_latency_waitable;

rapidjson::Value desc_value(const chain_desc &desc,
                            rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value out(rapidjson::kObjectType);
  json::set(out, "swap_effect", static_cast<std::int64_t>(desc.swap_effect),
            alloc);
  json::set(out, "swap_effect_name", bench::swap_effect_name(desc.swap_effect),
            alloc);
  json::set(out, "buffer_count", static_cast<std::int64_t>(desc.buffer_count),
            alloc);
  json::set(out, "format", static_cast<std::int64_t>(desc.format), alloc);
  json::set(out, "width", static_cast<std::int64_t>(desc.width), alloc);
  json::set(out, "height", static_cast<std::int64_t>(desc.height), alloc);
  json::set(out, "sample_count", static_cast<std::int64_t>(desc.sample_count),
            alloc);
  json::set(out, "sample_quality",
            static_cast<std::int64_t>(desc.sample_quality), alloc);
  json::set(out, "usage", hex(desc.buffer_usage), alloc);
  json::set(out, "flags", hex(desc.flags), alloc);
  json::set(out, "windowed", desc.windowed, alloc);
  return out;
}

std::string effect_text(std::uint32_t swap_effect) {
  switch (swap_effect) {
  case dxgi::effect_discard:
    return "DISCARD";
  case dxgi::effect_sequential:
    return "SEQUENTIAL";
  case dxgi::effect_flip_sequential:
    return "FLIP_SEQUENTIAL";
  case dxgi::effect_flip_discard:
    return "FLIP_DISCARD";
  default:
    return "swap effect " + std::to_string(swap_effect);
  }
}
} // namespace

bool requested(const std::vector<std::string> &args) {
  return has_launch_flag(args, flag) ||
         has_launch_flag(args, "-" + std::string(flag));
}

std::string_view to_string(outcome value) {
  switch (value) {
  case outcome::not_requested:
    return "off";
  case outcome::applied:
    return "applied";
  case outcome::already_flip:
    return "already_flip";
  case outcome::not_discard:
    return "not_discard";
  case outcome::buffer_count:
    return "buffer_count";
  case outcome::multisampled:
    return "multisampled";
  case outcome::srgb_format:
    return "srgb_format";
  case outcome::format:
    return "format";
  case outcome::usage:
    return "usage";
  case outcome::flags:
    return "flags";
  case outcome::hooks_not_ready:
    return "hooks_not_ready";
  case outcome::hook_failed:
    return "hook_failed";
  case outcome::create_failed:
    return "create_failed";
  case outcome::not_seen:
    return "not_seen";
  }
  return "unknown";
}

std::string_view explain(outcome value) {
  switch (value) {
  case outcome::not_requested:
    return "the game started without -xerces-flip-model";
  case outcome::applied:
    return "the chain was created as DXGI_SWAP_EFFECT_FLIP_DISCARD";
  case outcome::already_flip:
    return "the engine asked for a flip-model chain itself";
  case outcome::not_discard:
    return "the engine asked for DXGI_SWAP_EFFECT_SEQUENTIAL, which keeps "
           "old frames; only DISCARD is changed";
  case outcome::buffer_count:
    return "flip model needs 2 to 16 buffers";
  case outcome::multisampled:
    return "flip model cannot have multisampled buffers";
  case outcome::srgb_format:
    return "flip model cannot have an sRGB buffer format, and Xerces does not "
           "swap in the plain format because the picture could change";
  case outcome::format:
    return "flip model does not take this buffer format";
  case outcome::usage:
    return "the buffers have a usage flip model may not accept";
  case outcome::flags:
    return "the chain has flags flip model may not accept";
  case outcome::hooks_not_ready:
    return "the Present and ResizeBuffers detours were not both in place "
           "(ResizeBuffers follows the download overlay's Present detour)";
  case outcome::hook_failed:
    return "a detour could not be installed";
  case outcome::create_failed:
    return "DXGI refused the flip-model description, so the chain was "
           "created as the engine asked";
  case outcome::not_seen:
    return "the game's swap chain was created before the detour was in place";
  }
  return "unknown";
}

std::string_view to_string(arm_step value) {
  switch (value) {
  case arm_step::create_hook:
    return "post_unpack";
  case arm_step::present_hook:
    return "present_hook";
  }
  return "unknown";
}

namespace {
// Both detours ResizeBuffers waits for are in, with the flag, and nothing
// failed: the caller installs it, once.
bool hand_out_resize(arming &state, arm_step step) {
  if (!state.requested || !state.create_hooked || !state.present_hooked ||
      state.resize_due || !state.error.empty()) {
    return false;
  }
  state.resize_due = true;
  state.armed_by = step;
  return true;
}

void keep_first_error(arming &state, std::string_view error) {
  if (state.error.empty()) {
    state.error =
        error.empty() ? std::string("unknown error") : std::string(error);
  }
}
} // namespace

bool arm_create_hook(arming &state, bool hooked, std::string_view error) {
  state.requested = true;
  state.create_hooked = hooked;
  if (!hooked) {
    keep_first_error(state, error);
    return false;
  }
  return hand_out_resize(state, arm_step::create_hook);
}

bool arm_present_hook(arming &state) {
  state.present_hooked = true;
  return hand_out_resize(state, arm_step::present_hook);
}

void arm_resize_hook(arming &state, bool hooked, std::string_view error) {
  if (!state.resize_due) {
    return; // never handed out
  }
  state.resize_hooked = hooked;
  if (!hooked) {
    keep_first_error(state, error);
  }
}

std::optional<outcome> not_armed(const arming &state) {
  if (!state.error.empty()) {
    return outcome::hook_failed;
  }
  if (state.requested && state.create_hooked && state.present_hooked &&
      state.resize_hooked) {
    return std::nullopt;
  }
  return outcome::hooks_not_ready;
}

std::string missing(const arming &state) {
  if (!state.error.empty()) {
    return state.error;
  }
  if (!state.create_hooked) {
    return "the CreateSwapChain detour is not in place";
  }
  if (!state.present_hooked) {
    return "the overlay's Present detour is not in place";
  }
  if (!state.resize_hooked) {
    return "the ResizeBuffers detour is not in place yet";
  }
  return {};
}

decision decide(const chain_desc &engine, bool tearing_supported) {
  decision out;
  out.desc = engine;
  const auto keep = [&](outcome why) {
    out.result = why;
    return out;
  };
  if (engine.swap_effect == dxgi::effect_flip_sequential ||
      engine.swap_effect == dxgi::effect_flip_discard) {
    return keep(outcome::already_flip);
  }
  if (engine.swap_effect != dxgi::effect_discard) {
    return keep(outcome::not_discard);
  }
  if (engine.buffer_count < 2 || engine.buffer_count > dxgi::max_buffers) {
    return keep(outcome::buffer_count);
  }
  if (engine.sample_count != 1 || engine.sample_quality != 0) {
    return keep(outcome::multisampled);
  }
  if (srgb(engine.format)) {
    return keep(outcome::srgb_format);
  }
  if (!flip_format(engine.format)) {
    return keep(outcome::format);
  }
  if ((engine.buffer_usage & ~flip_usage) != 0) {
    return keep(outcome::usage);
  }
  if ((engine.flags & ~flip_flags) != 0) {
    return keep(outcome::flags);
  }
  out.result = outcome::applied;
  out.desc.swap_effect = dxgi::effect_flip_discard;
  if (tearing_supported) {
    out.desc.flags |= dxgi::chain_allow_tearing;
  }
  out.tearing = (out.desc.flags & dxgi::chain_allow_tearing) != 0;
  return out;
}

resize_request resize(const resize_request &engine,
                      std::uint32_t created_flags) {
  resize_request out = engine;
  out.flags =
      (engine.flags & ~creation_flags) | (created_flags & creation_flags);
  if (out.buffer_count == 1) {
    out.buffer_count = 2;
  }
  return out;
}

std::uint32_t present_flags(std::uint32_t flags, std::uint32_t sync_interval,
                            bool chain_tears, bool exclusive_fullscreen) {
  if (!chain_tears || sync_interval != 0 || exclusive_fullscreen ||
      (flags & dxgi::present_test) != 0) {
    return flags;
  }
  return flags | dxgi::present_allow_tearing;
}

std::string message(const report &r) {
  if (r.result == outcome::not_requested) {
    return "Flip model is off: " + std::string(explain(r.result));
  }
  const std::string asked =
      r.asked ? effect_text(r.asked->swap_effect) : std::string();
  if (r.applied() && r.made) {
    std::string out = "Flip model on: the game's swap chain is " +
                      effect_text(r.made->swap_effect) + " (the engine asked " +
                      "for " + asked + "), " +
                      std::to_string(r.made->buffer_count) + " buffers, ";
    out += r.tearing ? "tearing allowed with V-Sync off"
                     : "no tearing (the system does not support it)";
    return out;
  }
  std::string out = "Flip model was asked for but is not used: " +
                    std::string(explain(r.result));
  if (r.flip_hr) {
    out += " (" + hex(*r.flip_hr) + ")";
  }
  if (!r.error.empty()) {
    out += " (" + r.error + ")";
  }
  if (r.create_hr) {
    out += ". The engine's own swap chain call failed too (" +
           hex(*r.create_hr) + ")";
  } else if (r.asked) {
    out += ". The game presents as before (" + asked + ")";
  } else {
    out += ". The game presents as before";
  }
  return out;
}

std::string desc_json(const chain_desc &desc) {
  rapidjson::Document doc;
  return json::canonical(desc_value(desc, doc.GetAllocator()));
}

std::string data_json(const report &r) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "requested", r.result != outcome::not_requested, alloc);
  json::set(doc, "applied", r.applied(), alloc);
  json::set(doc, "reason", to_string(r.result), alloc);
  json::set(doc, "why", explain(r.result), alloc);
  json::set(doc, "chain", static_cast<std::int64_t>(r.chain), alloc);
  if (r.asked) {
    json::set(doc, "asked", desc_value(*r.asked, alloc), alloc);
  }
  if (r.made) {
    json::set(doc, "made", desc_value(*r.made, alloc), alloc);
  }
  json::set(doc, "tearing_supported", r.tearing_supported, alloc);
  json::set(doc, "tearing", r.tearing, alloc);
  if (r.flip_hr) {
    json::set(doc, "flip_hr", hex(*r.flip_hr), alloc);
  }
  if (r.create_hr) {
    json::set(doc, "create_hr", hex(*r.create_hr), alloc);
  }
  if (!r.error.empty()) {
    json::set(doc, "error", json::valid_utf8(r.error, 512), alloc);
  }
  return json::canonical(doc);
}

std::string resize_failure_json(const resize_request &asked,
                                const resize_request &given,
                                std::uint32_t width, std::uint32_t height,
                                std::uint32_t hr) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "hr", hex(hr), alloc);
  json::set(doc, "width", static_cast<std::int64_t>(width), alloc);
  json::set(doc, "height", static_cast<std::int64_t>(height), alloc);
  const auto request = [&](const resize_request &value) {
    rapidjson::Value out(rapidjson::kObjectType);
    json::set(out, "buffer_count",
              static_cast<std::int64_t>(value.buffer_count), alloc);
    json::set(out, "format", static_cast<std::int64_t>(value.format), alloc);
    json::set(out, "flags", hex(value.flags), alloc);
    return out;
  };
  json::set(doc, "asked", request(asked), alloc);
  json::set(doc, "given", request(given), alloc);
  return json::canonical(doc);
}
} // namespace xerces_core::flip_model
