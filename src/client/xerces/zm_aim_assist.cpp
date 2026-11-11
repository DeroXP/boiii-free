#include <std_include.hpp>

// xerces_zm_aim_assist: controller aim assist for Zombies that works like the
// newer Call of Duty games' (Modern Warfare III / Black Ops 6), docs/DESIGN.md
// 7.11. The game's own assist is weak on PC and its tuning dvars are not in the
// released game, so this one works on the view angles itself, every frame, on
// this PC. The rules and numbers are the ones the user's aim trainer documents
// for those games (AimTuner: README "Aim assist", aimtuner/codkit.py TUNE and
// AimAssist, style "classic"), from the community's measurements:
//
// - slowdown: while a zombie is inside a bubble around the crosshair (a fixed
//   area of the screen), your own look speed is cut, by 50% at full strength,
//   0.6 of that from the hip;
// - rotational: the view turns by itself to follow 60% of the zombie's motion
//   across your screen, whether it moved or you strafed (0.7 of that up and
//   down). It needs stick input: the left stick at least a quarter over
//   sideways, or the right stick at least 5% in any direction;
// - both need a clear line of sight and fade with distance: slowdown is full
//   from 3 m to 50 m and gone at 100 m, rotation fades in to full at 6 m, is
//   full to 35 m and all but gone past 70 m.
//
// There is no pull toward a target and no snap when aiming down sights: those
// games have neither. Only zombies (actors) count. It is off in multiplayer
// and campaign, in menus and the console, while the window is not in front,
// and without a controller (it reads the sticks itself, so a mouse never gets
// it). xerces_zm_aimAssist: 0 the game's own only, 1 those games' strength, 2
// the same rules a step stronger (80% rotation, 60% slowdown); saved on this
// PC.
//
// Where the game keeps things was measured in the running game on 2026-11-11
// (xerces_aim_debug), because Xerces' layouts of this data are not exact:
// - the camera (refdef) in cg: its place at cg+0x131D78, the tangents of half
//   the field of view 16 and 12 bytes before it, its forward axis 28 after;
// - the client's own view angles (what the sticks and the mouse turn): 0xB8C8
//   into the client's data, whose pointer the game hides, so it is noted as
//   the game allocates it (by its size: the released game passes no names).
// Every frame these are checked before anything is moved: the camera must be
// over the player's own entity with a unit forward axis and sane tangents,
// and the client's angles must be readable numbers. If they are not (another
// game build), the assist stays off and says so once in the log.
//
// No game function is called from here except the sight trace: two of them
// (CG_IsFullyInitialized, and anything that waits) froze a level load.

#include <component/command.hpp>
#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>

#include "runtime.hpp"

#include <utils/hook.hpp>

#include <Xinput.h>

namespace xerces_zm_aim_assist {
namespace {
using game::vec3_t;

// ------------------------------------------------------------------ rules
struct rules {
  float rotation; // share of the target's motion across the view followed
  float slowdown; // share of your own look speed taken away in the bubble
};
constexpr rules GAME_STRENGTH{0.60f, 0.50f}; // "roughly 60%", "roughly 50%"
constexpr rules STRONGER{0.80f, 0.60f};

constexpr float HIP_SLOWDOWN = 0.6f;   // slowdown from the hip, of the above
constexpr float ROTATION_PITCH = 0.7f; // the vertical share of rotation
constexpr float STRAFE_MIN = 0.25f;    // left stick sideways that counts
constexpr float AIM_MIN = 0.05f;       // right stick that counts
// The bubble: half its width and height in screen units (-1 to 1), all of the
// assist inside ZONE_FULL of it and none at its edge
constexpr float BUBBLE_X = 0.14f;
constexpr float BUBBLE_Y = 0.19f;
constexpr float ZONE_FULL = 0.75f;
constexpr float UNITS_PER_METRE = 39.37f;
// The sights count as up when the view is this much narrower than from the hip
constexpr float SIGHTS_UP_FOV = 0.93f;

struct ramp_point {
  float metres;
  float gain;
};
constexpr ramp_point RANGE_SLOWDOWN[] = {
    {0.0f, 0.0f}, {3.0f, 1.0f}, {50.0f, 1.0f}, {100.0f, 0.0f}};
constexpr ramp_point RANGE_ROTATION[] = {{0.0f, 0.0f},   {6.0f, 1.0f},
                                         {35.0f, 1.0f},  {50.0f, 0.3f},
                                         {70.0f, 0.05f}, {95.0f, 0.0f}};

template <size_t N> float ramp(float x, const ramp_point (&points)[N]) {
  if (x <= points[0].metres) {
    return points[0].gain;
  }
  for (size_t i = 1; i < N; ++i) {
    if (x <= points[i].metres) {
      const ramp_point &a = points[i - 1];
      const ramp_point &b = points[i];
      return a.gain + (b.gain - a.gain) * (x - a.metres) / (b.metres - a.metres);
    }
  }
  return points[N - 1].gain;
}

float zone(float e) {
  return e <= ZONE_FULL ? 1.0f
                        : (e >= 1.0f ? 0.0f : (1.0f - e) / (1.0f - ZONE_FULL));
}

// Test only: XERCES_AIM_TEST=1 acts as a controller with the left stick held
// sideways (and the game's window need not be in front), and logs what the
// assist did each second
bool test_pad = false;

// ------------------------------------------------------------- controller
using xinput_get_state_t = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);
xinput_get_state_t xinput_get_state = nullptr;

void load_xinput() {
  for (const wchar_t *name :
       {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
    if (HMODULE module = LoadLibraryW(name)) {
      xinput_get_state = reinterpret_cast<xinput_get_state_t>(
          GetProcAddress(module, "XInputGetState"));
      if (xinput_get_state) {
        return;
      }
    }
  }
}

struct pad_state {
  bool connected = false;
  float strafe = 0.0f; // the left stick, sideways, 0 to 1
  float aim = 0.0f;    // the right stick, any direction, 0 to 1
  bool active = false; // any input at all
};

pad_state read_pad() {
  pad_state out;
  if (test_pad) {
    out.connected = true;
    out.strafe = 1.0f;
    out.active = true;
    return out;
  }
  if (!xinput_get_state) {
    return out;
  }
  for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
    XINPUT_STATE state{};
    if (xinput_get_state(index, &state) != ERROR_SUCCESS) {
      continue;
    }
    out.connected = true;
    const XINPUT_GAMEPAD &pad = state.Gamepad;
    const float strafe = std::abs(static_cast<float>(pad.sThumbLX)) / 32767.0f;
    const float rx = static_cast<float>(pad.sThumbRX) / 32767.0f;
    const float ry = static_cast<float>(pad.sThumbRY) / 32767.0f;
    const float aim = std::min(1.0f, std::sqrt(rx * rx + ry * ry));
    const float ly = std::abs(static_cast<float>(pad.sThumbLY)) / 32767.0f;
    out.strafe = std::max(out.strafe, std::min(1.0f, strafe));
    out.aim = std::max(out.aim, aim);
    out.active = out.active || strafe > 0.2f || ly > 0.2f || aim > 0.2f ||
                 pad.bLeftTrigger > 40 || pad.bRightTrigger > 40 ||
                 pad.wButtons != 0;
  }
  return out;
}

// ------------------------------------------------------ the client's angles
using client_active =
    std::remove_reference_t<decltype((*game::cl::clients)->actives[0])>;
constexpr size_t CLIENT_ANGLES = 0xB8C8; // pitch, then yaw; see the top
utils::hook::detour hunk_alloc_hook;
std::atomic<uint8_t *> client_data{nullptr};

void *hunk_alloc_stub(game::hunk::HunkUser *user, size_t size,
                      int32_t alignment, const char *name) {
  void *result = hunk_alloc_hook.invoke<void *>(user, size, alignment, name);
  // One client's data for each local client (two on PC). Nothing else here
  // may wait or write a log: the game calls this while it loads.
  if (result && size >= sizeof(client_active) &&
      size <= 4 * sizeof(client_active) && size % sizeof(client_active) == 0) {
    client_data.store(static_cast<uint8_t *>(result),
                      std::memory_order_release);
  }
  return result;
}

bool writable(const void *address, size_t size) {
  MEMORY_BASIC_INFORMATION info{};
  if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
      (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) == 0) {
    return false;
  }
  const auto begin = reinterpret_cast<uintptr_t>(address);
  const auto end =
      reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
  return begin + size <= end;
}

// The client's own pitch and yaw, or null while they cannot be trusted
float *client_angles() {
  uint8_t *data = client_data.load(std::memory_order_acquire);
  if (!data || !writable(data + CLIENT_ANGLES, 8)) {
    return nullptr;
  }
  auto *angles = reinterpret_cast<float *>(data + CLIENT_ANGLES);
  if (!std::isfinite(angles[0]) || !std::isfinite(angles[1]) ||
      std::abs(angles[0]) > 360.0f || std::abs(angles[1]) > 1.0e7f) {
    return nullptr;
  }
  return angles;
}

// ------------------------------------------------------------- the camera
constexpr size_t CAMERA_IN_CG = 0x131D78; // see the top
constexpr float RAD = 57.2957795f;

struct camera {
  vec3_t eye{};
  float pitch = 0.0f; // the game's angles: down is positive
  float yaw = 0.0f;
  float tan_x = 1.0f; // tangents of half the field of view
  float tan_y = 0.5625f;
};

float normalize(float angle) {
  while (angle > 180.0f) {
    angle -= 360.0f;
  }
  while (angle < -180.0f) {
    angle += 360.0f;
  }
  return angle;
}

// The camera the game drew the last frame with, if it reads as one over the
// local player's own entity
bool read_camera(camera &out, int &client) {
  const auto &cg = game::cg::cgArray[0];
  client = static_cast<int>(cg.clientNum);
  if (client < 0 || client > 17) {
    return false;
  }
  const auto *base = reinterpret_cast<const uint8_t *>(&cg) + CAMERA_IN_CG;
  float tangents[2];
  float eye[3];
  float forward[3];
  std::memcpy(tangents, base - 16, sizeof(tangents));
  std::memcpy(eye, base, sizeof(eye));
  std::memcpy(forward, base + 28, sizeof(forward));
  const float length = std::sqrt(forward[0] * forward[0] +
                                 forward[1] * forward[1] +
                                 forward[2] * forward[2]);
  const auto &self = game::cg::cg_entitiesArray[0].entities[client];
  const float dx = eye[0] - self.pose.origin.x;
  const float dy = eye[1] - self.pose.origin.y;
  const float dz = eye[2] - self.pose.origin.z;
  if (!(tangents[0] > 0.2f && tangents[0] < 4.0f && tangents[1] > 0.1f &&
        tangents[1] < 4.0f && length > 0.98f && length < 1.02f &&
        self.nextValid && dx * dx + dy * dy < 150.0f * 150.0f && dz > -20.0f &&
        dz < 150.0f)) {
    return false;
  }
  out.eye.x = eye[0];
  out.eye.y = eye[1];
  out.eye.z = eye[2];
  out.yaw = std::atan2(forward[1], forward[0]) * RAD;
  out.pitch = -std::asin(std::clamp(forward[2], -1.0f, 1.0f)) * RAD;
  out.tan_x = tangents[0];
  out.tan_y = tangents[1];
  return true;
}

// ------------------------------------------------------------- game state
bool in_zombies_level() {
  return game::com::Com_SessionMode_IsMode(game::eModes::ZOMBIES) &&
         game::com::Com_IsInGame() && !game::com::Com_IsRunningUILevel() &&
         game::cg::clientUIActives->actives[0].connectionState ==
             game::connstate_t::ACTIVE &&
         game::cg::clientUIActives->actives[0].keyCatchers == 0 &&
         game::cg::cgArray[0].snap && game::cg::cgArray[0].nextSnap;
}

bool visible(int self, const vec3_t &eye, const vec3_t &point) {
  game::cm::trace_t trace{};
  trace.fraction = 1.0f;
  int hit = self;
  constexpr game::contents_t SOLID = 1;
  game::cg::CG_SightTracePoint(&hit, &eye, &point, SOLID, &trace);
  return trace.fraction >= 0.97f;
}

// ------------------------------------------------------------------ state
// Where each zombie was seen from the eye last frame
struct seen {
  uint32_t frame = 0;
  float yaw = 0.0f;
  float pitch = 0.0f;
};

constexpr int ENTITIES = 0x700;

struct state {
  std::array<seen, ENTITIES> tracks{};
  uint32_t frame = 0;
  float client_pitch = 0.0f; // the client's own angles as this left them
  float client_yaw = 0.0f;
  bool have_client = false;
  float hip_tan_x = 0.0f; // the widest view seen in this level: from the hip
  bool warned = false;
  int64_t last_pad_ms = 0;
  int64_t last_qpc = 0;
  // what the last frame did, for xerces_aim_debug and the test log
  int target = -1;
  float target_e = 0.0f;
  float target_metres = 0.0f;
  float strength = 0.0f;
  float slow = 0.0f;
  bool sights_up = false;
  // sums since the last test log line
  double sum_motion = 0.0;   // degrees the target moved across the view
  double sum_rotation = 0.0; // degrees the view was turned to follow
  int frames_in_bubble = 0;
  int64_t last_log_ms = 0;
};
state st;

int64_t now_ms() { return static_cast<int64_t>(GetTickCount64()); }

void reset() {
  st.have_client = false;
  st.target = -1;
  st.strength = 0.0f;
  st.slow = 0.0f;
}

void warn_once(const char *what) {
  if (st.warned) {
    return;
  }
  st.warned = true;
  xerces_runtime::log_warn("xerces.aim_assist.off",
                           std::string("Aim assist is off: ") + what +
                               " is not where this build of Xerces looks for "
                               "it (another game build?)");
}

struct candidate {
  int index = -1;
  float e = 0.0f; // where it is in the bubble: 0 the middle, 1 the edge
  float dist = 0.0f;
  float yaw = 0.0f;
  float pitch = 0.0f;
  bool had_previous = false;
  float previous_yaw = 0.0f;
  float previous_pitch = 0.0f;
  vec3_t chest{};
  vec3_t head{};
};

// The zombie nearest the middle of the bubble (up to half as far again out as
// its edge), and every zombie's place this frame noted for the next
candidate find_target(const camera &cam) {
  candidate best;
  const float view_elevation = -cam.pitch;
  const float cos_pitch = std::cos(cam.pitch / RAD);
  auto &entities = game::cg::cg_entitiesArray[0].entities;
  for (int i = 0; i < ENTITIES; ++i) {
    const auto &cent = entities[i];
    if (!cent.nextValid ||
        static_cast<uint8_t>(cent.pose.eType) != 0x12 /* ET_ACTOR */) {
      continue;
    }
    const vec3_t &origin = cent.pose.origin;
    // its height from its bounds (in the world when their bottom is near the
    // origin), 72 units if they look wrong
    float height = cent.pose.absmax.z - cent.pose.absmin.z;
    if (height < 20.0f || height > 160.0f) {
      height = 72.0f;
    }
    const float bottom = std::abs(cent.pose.absmin.z - origin.z) < 100.0f
                             ? cent.pose.absmin.z
                             : origin.z;
    const float dx = origin.x - cam.eye.x;
    const float dy = origin.y - cam.eye.y;
    const float flat = std::sqrt(dx * dx + dy * dy);
    const float chest_z = bottom + height * 0.67f;
    const float dz = chest_z - cam.eye.z;
    const float dist = std::sqrt(flat * flat + dz * dz);
    if (dist < 2.0f || dist > 100.0f * UNITS_PER_METRE) {
      continue;
    }
    const float yaw = std::atan2(dy, dx) * RAD;
    const float pitch = -std::atan2(dz, flat) * RAD;
    seen &track = st.tracks[i];
    const bool had_previous = track.frame + 1 == st.frame;
    const float previous_yaw = track.yaw;
    const float previous_pitch = track.pitch;
    track = {st.frame, yaw, pitch};

    // where it is on screen from the crosshair: sideways to its middle line,
    // and up and down nothing anywhere between its feet and the top of its head
    const float ex = std::abs(normalize(yaw - cam.yaw)) * cos_pitch;
    const float low = std::atan2(bottom + 10.0f - cam.eye.z, flat) * RAD;
    const float high = std::atan2(bottom + height - cam.eye.z, flat) * RAD;
    const float ev = view_elevation >= low && view_elevation <= high
                         ? 0.0f
                         : std::min(std::abs(view_elevation - low),
                                    std::abs(view_elevation - high));
    if (ex > 60.0f || ev > 60.0f) {
      continue;
    }
    const float sx = std::tan(ex / RAD) / cam.tan_x / BUBBLE_X;
    const float sy = std::tan(ev / RAD) / cam.tan_y / BUBBLE_Y;
    const float e = std::sqrt(sx * sx + sy * sy);
    if (e > 1.5f || (best.index >= 0 && e >= best.e)) {
      continue;
    }
    best.index = i;
    best.e = e;
    best.dist = dist;
    best.yaw = yaw;
    best.pitch = pitch;
    best.had_previous = had_previous;
    best.previous_yaw = previous_yaw;
    best.previous_pitch = previous_pitch;
    best.chest = origin;
    best.chest.z = chest_z;
    best.head = origin;
    best.head.z = bottom + height * 0.92f;
  }
  return best;
}

void test_log() {
  if (!test_pad || now_ms() - st.last_log_ms < 1000) {
    return;
  }
  st.last_log_ms = now_ms();
  char text[256];
  std::snprintf(text, sizeof(text),
                "target %d at %.1f m, e %.2f, rotation %.2f, slowdown %.2f; "
                "last second: %d frames in the bubble, target moved %.2f deg, "
                "view followed %.2f deg",
                st.target, st.target_metres, st.target_e, st.strength, st.slow,
                st.frames_in_bubble, st.sum_motion, st.sum_rotation);
  xerces_runtime::log_info("xerces.aim_assist.test", text);
  st.sum_motion = st.sum_rotation = 0.0;
  st.frames_in_bubble = 0;
}

void frame() {
  const auto level = game::get_dvar_int("xerces_zm_aimAssist");
  if (!level || *level <= 0) {
    reset();
    return;
  }
  const rules &r = *level >= 2 ? STRONGER : GAME_STRENGTH;

  const pad_state pad = read_pad();
  if (pad.active) {
    st.last_pad_ms = now_ms();
  }
  LARGE_INTEGER qpc{};
  LARGE_INTEGER freq{};
  QueryPerformanceCounter(&qpc);
  QueryPerformanceFrequency(&freq);
  const float dt = st.last_qpc == 0
                       ? 0.0f
                       : static_cast<float>(qpc.QuadPart - st.last_qpc) /
                             static_cast<float>(freq.QuadPart);
  st.last_qpc = qpc.QuadPart;

  if (!in_zombies_level()) {
    st.hip_tan_x = 0.0f;
    reset();
    return;
  }
  if (!pad.connected || now_ms() - st.last_pad_ms > 2000) {
    reset();
    return;
  }
  if (!test_pad) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) {
      reset();
      return;
    }
  }

  camera cam;
  int client = 0;
  if (!read_camera(cam, client)) {
    warn_once("the camera");
    reset();
    return;
  }
  float *view = client_angles();
  if (!view) {
    warn_once("the client's view angles");
    reset();
    return;
  }

  ++st.frame;
  // your own turn since this last left the view
  float own_pitch = 0.0f;
  float own_yaw = 0.0f;
  if (st.have_client) {
    own_pitch = normalize(view[0] - st.client_pitch);
    own_yaw = normalize(view[1] - st.client_yaw);
  }
  st.hip_tan_x = std::max(st.hip_tan_x, cam.tan_x);
  st.sights_up = cam.tan_x < st.hip_tan_x * SIGHTS_UP_FOV;

  float add_pitch = 0.0f;
  float add_yaw = 0.0f;
  st.target = -1;
  st.strength = 0.0f;
  st.slow = 0.0f;
  const candidate found = find_target(cam);
  if (found.index >= 0 && dt > 0.0f && dt < 0.25f &&
      (visible(client, cam.eye, found.chest) ||
       visible(client, cam.eye, found.head))) {
    const float metres = found.dist / UNITS_PER_METRE;
    const float in_zone = zone(found.e);
    float slow = r.slowdown * in_zone * ramp(metres, RANGE_SLOWDOWN);
    if (!st.sights_up) {
      slow *= HIP_SLOWDOWN;
    }
    const bool input = pad.strafe >= STRAFE_MIN || pad.aim >= AIM_MIN;
    const float strength =
        input ? r.rotation * in_zone * ramp(metres, RANGE_ROTATION) : 0.0f;

    // how far the target moved across the view this frame; a jump of more
    // than 500 degrees a second is a respawn, not a move
    float moved_yaw = 0.0f;
    float moved_pitch = 0.0f;
    if (found.had_previous) {
      moved_yaw = normalize(found.yaw - found.previous_yaw);
      moved_pitch = normalize(found.pitch - found.previous_pitch);
      if (std::abs(moved_yaw) > 500.0f * dt ||
          std::abs(moved_pitch) > 500.0f * dt) {
        moved_yaw = moved_pitch = 0.0f;
      }
    }
    add_yaw += strength * moved_yaw;
    add_pitch += strength * moved_pitch * ROTATION_PITCH;
    // slowdown acts on what the right stick turned, not on recoil
    if (pad.aim >= AIM_MIN && slow > 0.0f) {
      add_yaw -= own_yaw * slow;
      add_pitch -= own_pitch * slow;
    }
    st.target = found.index;
    st.target_e = found.e;
    st.target_metres = metres;
    st.strength = strength;
    st.slow = slow;
    st.sum_motion += std::abs(moved_yaw);
    st.sum_rotation += std::abs(strength * moved_yaw);
    if (in_zone > 0.0f) {
      ++st.frames_in_bubble;
    }
  }

  // a frame's change is never more than 10 degrees
  add_pitch = std::clamp(add_pitch, -10.0f, 10.0f);
  add_yaw = std::clamp(add_yaw, -10.0f, 10.0f);
  view[0] += add_pitch;
  view[1] += add_yaw;
  st.client_pitch = view[0];
  st.client_yaw = view[1];
  st.have_client = true;
  test_log();
}

// What the assist sees right now: to the console and the log
void debug() {
  const pad_state pad = read_pad();
  char line[420];
  std::snprintf(line, sizeof(line),
                "aim assist: setting %d, controller %s (left stick sideways "
                "%.2f, right stick %.2f), in a Zombies level %s",
                game::get_dvar_int("xerces_zm_aimAssist").value_or(-1),
                pad.connected ? "found" : "NOT found", pad.strafe, pad.aim,
                in_zombies_level() ? "yes" : "no");
  std::string text = line;
  if (in_zombies_level()) {
    camera cam;
    int client = 0;
    const bool camera_ok = read_camera(cam, client);
    const float *view = client_angles();
    std::snprintf(line, sizeof(line),
                  "\ncamera %s: eye %.1f %.1f %.1f, pitch %.2f, yaw %.2f, "
                  "tangents %.3f %.3f, sights %s\nclient angles %s: %.2f %.2f"
                  "\ntarget %d at %.1f m, e %.2f, rotation %.2f, slowdown %.2f",
                  camera_ok ? "ok" : "NOT ok", cam.eye.x, cam.eye.y, cam.eye.z,
                  cam.pitch, cam.yaw, cam.tan_x, cam.tan_y,
                  st.sights_up ? "up" : "down", view ? "ok" : "NOT ok",
                  view ? view[0] : 0.0f, view ? view[1] : 0.0f, st.target,
                  st.target_metres, st.target_e, st.strength, st.slow);
    text += line;
  }
  xerces_runtime::log_info("xerces.aim_assist.debug", text);
  printf("%s\n", text.c_str());
}
} // namespace

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_zm_aim_assist");

public:
  void post_unpack() override {
    if (!game::is_client() || !game::is_new_client()) {
      return;
    }
    const game::DvarFlags saved{.archive = 1};
    game::register_dvar_int(
        "xerces_zm_aimAssist", 0, 0, 2, saved,
        "Zombies only, on this PC: controller aim assist like the newer Call "
        "of Duty games' (slowdown and rotational). 0 the game's own, 1 those "
        "games' strength, 2 a step stronger.");
    load_xinput();
    wchar_t buffer[4];
    test_pad = GetEnvironmentVariableW(L"XERCES_AIM_TEST", buffer, 4) > 0 &&
               buffer[0] == L'1';
    hunk_alloc_hook.create(game::hunk::Hunk_UserAlloc.get(), hunk_alloc_stub);
    scheduler::loop(frame, scheduler::main);
    command::add("xerces_aim_debug", [] { debug(); });
  }
};
} // namespace xerces_zm_aim_assist

REGISTER_COMPONENT(xerces_zm_aim_assist::component)
