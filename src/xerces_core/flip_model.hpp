#pragma once

// Flip-model presentation for the game's swap chain (docs/DESIGN.md 12.4 item
// 4): the rules the client's flip_model component
// (src/client/xerces/flip_model.cpp) applies, kept here so they can be tested
// without the game or DXGI.
//
// The engine creates its swap chain through IDXGIFactory::CreateSwapChain
// with DXGI_SWAP_EFFECT_DISCARD, the blt model: in a window or borderless,
// every frame is copied into the desktop compositor's surface, which exclusive
// fullscreen avoids. With the launch flag -xerces-flip-model the client asks
// for DXGI_SWAP_EFFECT_FLIP_DISCARD instead when the engine's description
// allows it, and adds DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING when the system
// supports tearing. ResizeBuffers then has to keep that flag, and Present may
// pass DXGI_PRESENT_ALLOW_TEARING with sync interval 0, outside exclusive
// fullscreen only.
//
// The numbers are DXGI's (dxgi.h, dxgiformat.h, dxgi1_5.h); the client
// checks them against the SDK's headers with static_assert.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core::flip_model {
// The launch flag, and its name as the launcher's settings handle flags
// (settings_options' has_flag and with_flag take names without the dash).
inline constexpr std::string_view flag = "-xerces-flip-model";
inline constexpr std::string_view flag_name = "xerces-flip-model";

// The flag before the first "+" command, spelled "-name" or "--name" (both
// count in the launcher's settings too).
bool requested(const std::vector<std::string> &args);

namespace dxgi {
// DXGI_SWAP_EFFECT
inline constexpr std::uint32_t effect_discard = 0;
inline constexpr std::uint32_t effect_sequential = 1;
inline constexpr std::uint32_t effect_flip_sequential = 3;
inline constexpr std::uint32_t effect_flip_discard = 4;
// DXGI_FORMAT: the four flip model allows, and the two sRGB ones it does not.
inline constexpr std::uint32_t format_r16g16b16a16_float = 10;
inline constexpr std::uint32_t format_r10g10b10a2_unorm = 24;
inline constexpr std::uint32_t format_r8g8b8a8_unorm = 28;
inline constexpr std::uint32_t format_r8g8b8a8_unorm_srgb = 29;
inline constexpr std::uint32_t format_b8g8r8a8_unorm = 87;
inline constexpr std::uint32_t format_b8g8r8a8_unorm_srgb = 91;
// DXGI_SWAP_CHAIN_FLAG
inline constexpr std::uint32_t chain_allow_mode_switch = 0x2;
inline constexpr std::uint32_t chain_frame_latency_waitable = 0x40;
inline constexpr std::uint32_t chain_allow_tearing = 0x800;
// DXGI_USAGE
inline constexpr std::uint32_t usage_shader_input = 0x10;
inline constexpr std::uint32_t usage_render_target_output = 0x20;
inline constexpr std::uint32_t usage_back_buffer = 0x40;
inline constexpr std::uint32_t usage_unordered_access = 0x400;
// DXGI_PRESENT
inline constexpr std::uint32_t present_test = 0x1;
inline constexpr std::uint32_t present_allow_tearing = 0x200;
// DXGI_MAX_SWAP_CHAIN_BUFFERS
inline constexpr std::uint32_t max_buffers = 16;
} // namespace dxgi

// The parts of DXGI_SWAP_CHAIN_DESC the decision reads or changes.
struct chain_desc {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t format = 0;
  std::uint32_t sample_count = 1;
  std::uint32_t sample_quality = 0;
  std::uint32_t buffer_usage = 0;
  std::uint32_t buffer_count = 0;
  bool windowed = true;
  std::uint32_t swap_effect = 0;
  std::uint32_t flags = 0;

  bool operator==(const chain_desc &) const = default;
};

// What became of a chain the engine created, and why.
enum class outcome {
  not_requested,   // the game started without -xerces-flip-model
  applied,         // created as DXGI_SWAP_EFFECT_FLIP_DISCARD
  already_flip,    // the engine asked for a flip model itself
  not_discard,     // DXGI_SWAP_EFFECT_SEQUENTIAL keeps old frames; left alone
  buffer_count,    // flip model needs 2 to 16 buffers
  multisampled,    // flip model has no multisampled buffers
  srgb_format,     // flip model has no sRGB buffers; skipped, never converted
  format,          // a buffer format flip model does not take
  usage,           // buffer usage bits flip model may not accept
  flags,           // chain flags flip model may not accept
  hooks_not_ready, // the Present and ResizeBuffers detours were not in place
  hook_failed,     // a detour could not be installed
  create_failed,   // DXGI refused the flip description; made as asked
  not_seen,        // the chain was made before the detour was in place
};
std::string_view to_string(outcome value);
// One plain sentence on why the chain was or was not changed.
std::string_view explain(outcome value);

struct decision {
  chain_desc desc; // what to create: the engine's, changed only if applied
  outcome result = outcome::not_requested;
  bool tearing = false; // desc has DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING added
  bool applied() const { return result == outcome::applied; }
};

// From the engine's description alone. tearing_supported is
// IDXGIFactory5::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING).
// Only DISCARD becomes FLIP_DISCARD; everything else the engine asked for
// (size, format, buffer count, usage, the other flags, windowed) is kept.
decision decide(const chain_desc &engine, bool tearing_supported);

// ResizeBuffers on a chain decide() changed. ALLOW_TEARING and
// FRAME_LATENCY_WAITABLE_OBJECT cannot change after creation, so they are
// taken from the creation flags; a buffer count of 1 becomes 2, flip model's
// least. 0 (keep the count) and the format pass through.
struct resize_request {
  std::uint32_t buffer_count = 0;
  std::uint32_t format = 0;
  std::uint32_t flags = 0;
  bool operator==(const resize_request &) const = default;
};
resize_request resize(const resize_request &engine,
                      std::uint32_t created_flags);

// Present on that chain: DXGI_PRESENT_ALLOW_TEARING is added only when the
// chain was created with ALLOW_TEARING, the sync interval is 0 and the chain
// is not in exclusive fullscreen (DXGI refuses it there), and never to a test
// Present (DXGI_PRESENT_TEST).
std::uint32_t present_flags(std::uint32_t flags, std::uint32_t sync_interval,
                            bool chain_tears, bool exclusive_fullscreen);

// xerces.flip_model, logged once for each chain the game creates.
struct report {
  outcome result = outcome::not_seen;
  std::uint32_t chain = 0;         // the game's chains so far, this one too
  std::optional<chain_desc> asked; // the engine's description
  std::optional<chain_desc> made;  // what was created (none if it failed)
  bool tearing_supported = false;
  bool tearing = false;                   // the chain has ALLOW_TEARING
  std::optional<std::uint32_t> flip_hr;   // the refused flip creation
  std::optional<std::uint32_t> create_hr; // the engine's own call failed
  std::string error;                      // why a detour failed
  bool applied() const { return result == outcome::applied; }
};
std::string message(const report &r);
std::string data_json(const report &r);
std::string desc_json(const chain_desc &desc);

// xerces.flip_model.resize_failed: ResizeBuffers failed on the flipped chain
// (logged once). What the engine asked, what was passed, and DXGI's answer.
std::string resize_failure_json(const resize_request &asked,
                                const resize_request &given,
                                std::uint32_t width, std::uint32_t height,
                                std::uint32_t hr);
} // namespace xerces_core::flip_model
