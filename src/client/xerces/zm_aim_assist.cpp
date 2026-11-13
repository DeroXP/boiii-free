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
//   area of the screen), your own look speed is cut;
// - rotational: the view turns by itself to follow a share of the zombie's
//   motion across your screen, whether it moved or you strafed. It needs
//   stick input: the left stick sideways, or the right stick at least 5% in
//   any direction;
// - both need a clear line of sight and fade with distance.
//
// There is no pull sideways toward a target and no snap when aiming down
// sights: those games have neither. Only zombies (actors) count, and only
// live ones the player can see (below). It is off in multiplayer
// and campaign, in menus and the console, while the window is not in front,
// and without a controller (it reads the sticks itself, so a mouse never gets
// it). xerces_zm_aimAssist: 0 the game's own only, 1 "Modern", 2 "Modern,
// Stronger"; saved on this PC.
//
// The numbers started as those games' own (60% rotation, 50% slowdown, 0.6 of
// the slowdown from the hip, rotation fading in from nothing at 0 m to full at
// 6 m). The user asked for more on 2026-11-12, and those numbers are made for
// players 10 to 40 m away, while zombies are fought from the hip at 1 to 5 m,
// where they left a fifth of the rotation. So, for Zombies: 75% rotation and
// 60% slowdown (95% and 75% for "Stronger"), all of it from 1.5 m (rotation)
// and 1 m (slowdown), 0.85 of the slowdown from the hip, a bubble a third
// larger, and the left stick counts from 15% sideways.
//
// The user played that (beta.24, 2026-11-13, with a controller) and found
// "Modern" too strong, not "Stronger": the request for more had been made
// with the setting off. So "Modern" is under those games' own numbers now
// (50% rotation, 40% slowdown, 0.65 of it from the hip, their bubble, all of
// it only from 3 m and 2 m), about half of what it was at 1 to 3 m; "Stronger"
// is what it was. From the same playtest:
// - a dying zombie is no target. Measured in the game (xerces_aim_actors): a
//   live zombie's state flags are 0x01; the frame it dies they are 0x10, a
//   quarter of a second later it is a ragdoll, a second later a corpse entity.
//   The view used to follow the body to the floor.
// - a zombie the player cannot see is no target. The sight check's "head" was
//   12 units above a zombie's real head (the box is 10 under the feet to 90
//   over them; a zombie is about 70 tall), so one wholly behind a low wall
//   or a car counted as seen. The points are the neck and the middle of the
//   head now, foliage and no-sight brushes block as walls do, and the nearest
//   zombie that is seen is taken, not the nearest whatever is in front of it.
// - the view settles on the neck, with leeway: between the middle of the
//   torso and the top of the head nothing moves the view up or down but the
//   player (head and torso are both theirs to choose); below or above that,
//   with a zombie in the bubble and the stick in use, the view drifts back to
//   the nearer edge. That much is a pull, up and down only, because the user
//   asked for it.
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
#include "safe_memory.hpp"

#include <utils/hook.hpp>

#include <Xinput.h>

namespace xerces_zm_aim_assist {
namespace {
using game::vec3_t;

// ------------------------------------------------------------------ rules
struct ramp_point {
  float metres;
  float gain;
};
struct rules {
  float rotation; // share of the target's motion across the view followed
  float slowdown; // share of your own look speed taken away in the bubble
  float hip;      // slowdown from the hip, of the above
  // The bubble: half its width and height in screen units (-1 to 1), all of
  // the assist inside ZONE_FULL of it and none at its edge
  float bubble_x;
  float bubble_y;
  // How much of each is left by distance
  const ramp_point *range_rotation;
  size_t range_rotation_count;
  const ramp_point *range_slowdown;
  size_t range_slowdown_count;
};
// Those games' own: slowdown from nothing at 0 m to full at 3 m; rotation
// from nothing at 0 m to full at 6 m, 0.3 at 50 m and 0.05 at 70 m.
// "Modern": a little of each up close, all of it from 2 m and 3 m.
constexpr ramp_point MODERN_SLOWDOWN[] = {
    {0.0f, 0.35f}, {2.0f, 1.0f}, {50.0f, 1.0f}, {100.0f, 0.0f}};
constexpr ramp_point MODERN_ROTATION[] = {{0.0f, 0.2f},   {3.0f, 1.0f},
                                          {35.0f, 1.0f},  {50.0f, 0.5f},
                                          {70.0f, 0.15f}, {95.0f, 0.0f}};
// "Stronger": what "Modern" was until 2026-11-13
constexpr ramp_point STRONGER_SLOWDOWN[] = {
    {0.0f, 0.5f}, {1.0f, 1.0f}, {50.0f, 1.0f}, {100.0f, 0.0f}};
constexpr ramp_point STRONGER_ROTATION[] = {{0.0f, 0.35f},  {1.5f, 1.0f},
                                            {35.0f, 1.0f},  {50.0f, 0.5f},
                                            {70.0f, 0.15f}, {95.0f, 0.0f}};
// Those games' own are 0.60 rotation, 0.50 slowdown, 0.6 from the hip and a
// bubble of 0.14 by 0.19; see the top for these
constexpr rules GAME_STRENGTH{0.50f,
                              0.40f,
                              0.65f,
                              0.15f,
                              0.20f,
                              MODERN_ROTATION,
                              std::size(MODERN_ROTATION),
                              MODERN_SLOWDOWN,
                              std::size(MODERN_SLOWDOWN)};
constexpr rules STRONGER{0.95f,
                         0.75f,
                         0.85f,
                         0.19f,
                         0.25f,
                         STRONGER_ROTATION,
                         std::size(STRONGER_ROTATION),
                         STRONGER_SLOWDOWN,
                         std::size(STRONGER_SLOWDOWN)};

constexpr float ROTATION_PITCH = 0.85f; // the vertical share of rotation
constexpr float STRAFE_MIN = 0.15f;     // left stick sideways that counts
constexpr float AIM_MIN = 0.05f;        // right stick that counts
constexpr float ZONE_FULL = 0.75f;

// A zombie, as shares of its box's height from the box's bottom. The box is
// 100 high, from 10 under the feet (measured, 2026-11-13); in units over the
// feet these are 6, 40, 57, 64, 68 and 70.
constexpr float BODY_BOTTOM = 0.16f; // the bubble's lower edge on it
constexpr float BODY_TORSO =
    0.50f; // the middle of the torso: the leeway's lower edge
constexpr float BODY_NECK = 0.67f;  // what the rotation follows
constexpr float BODY_HEAD = 0.74f;  // the middle of the head
constexpr float BODY_CROWN = 0.78f; // the leeway's upper edge
constexpr float BODY_TOP = 0.80f;   // the bubble's upper edge on it
// Outside the leeway the view drifts back to its nearer edge: this share of
// what is left a second, and never more than SETTLE_MAX degrees a second
constexpr float SETTLE_GAIN = 4.0f;
constexpr float SETTLE_MAX = 20.0f;
// A dead actor's state flags (lerp.eFlags), from the frame it dies
constexpr int32_t ACTOR_DEAD = 0x10;
// What blocks the player's sight: solid, foliage, and the brushes that stop
// AI seeing and bullets passing (1, 2, 0x1000, 0x2000)
constexpr uint32_t SIGHT_MASK = 0x3003;
constexpr float UNITS_PER_METRE = 39.37f;
// The sights count as up when the view is this much narrower than from the hip
constexpr float SIGHTS_UP_FOV = 0.93f;

float ramp(float x, const ramp_point *points, const size_t N) {
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
// assist did each second (XERCES_AIM_TEST=4: four times a second)
bool test_pad = false;
int64_t log_every_ms = 1000;

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

// The game's own stick dead zone (gpad_stick_deadzone_min, which the player
// can change; XInput's when the game has none): a stick at rest reports
// offsets of that order and the game ignores them. The raw values were
// compared with AIM_MIN (5%) and STRAFE_MIN, so a pad whose right stick rests
// a little off centre counted as aiming on every frame, and the view followed
// targets with no stick input (review, 2026-11-13).
float stick_dead_zone() {
  constexpr float xinput = XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE / 32767.0f;
  const float value =
      game::get_dvar_float("gpad_stick_deadzone_min").value_or(xinput);
  return std::isfinite(value) && value >= 0.0f && value <= 0.5f ? value
                                                                : xinput;
}

// How far past the dead zone, 0 to 1
float past_dead_zone(const float amount, const float dead) {
  return amount <= dead ? 0.0f
                        : std::min(1.0f, (amount - dead) / (1.0f - dead));
}

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
  // Only the slot that answered last is asked every frame: asking an empty
  // slot makes Windows look for devices, which costs far more than a read
  // (Microsoft's own advice), and three of the four are empty. The others are
  // tried again every two seconds while none answers.
  static int known_slot = -1;
  static uint64_t next_scan_ms = 0;
  const uint64_t now = GetTickCount64();
  const bool scan = known_slot < 0 && now >= next_scan_ms;
  if (known_slot < 0 && !scan) {
    return out;
  }
  if (scan) {
    next_scan_ms = now + 2000;
  }
  for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
    if (!scan && static_cast<int>(index) != known_slot) {
      continue;
    }
    XINPUT_STATE state{};
    if (xinput_get_state(index, &state) != ERROR_SUCCESS) {
      if (static_cast<int>(index) == known_slot) {
        known_slot = -1;
        next_scan_ms = now + 2000;
      }
      continue;
    }
    known_slot = static_cast<int>(index);
    out.connected = true;
    const XINPUT_GAMEPAD &pad = state.Gamepad;
    const float strafe = std::abs(static_cast<float>(pad.sThumbLX)) / 32767.0f;
    const float rx = static_cast<float>(pad.sThumbRX) / 32767.0f;
    const float ry = static_cast<float>(pad.sThumbRY) / 32767.0f;
    const float aim = std::min(1.0f, std::sqrt(rx * rx + ry * ry));
    const float ly = std::abs(static_cast<float>(pad.sThumbLY)) / 32767.0f;
    const float dead = stick_dead_zone();
    out.strafe = std::max(out.strafe, past_dead_zone(strafe, dead));
    out.aim = std::max(out.aim, past_dead_zone(aim, dead));
    out.active = out.active || strafe > 0.2f || ly > 0.2f || aim > 0.2f ||
                 pad.bLeftTrigger > 40 || pad.bRightTrigger > 40 ||
                 pad.wButtons != 0;
    break;
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

// Where the client's own pitch and yaw are, with what they are now; null
// while they cannot be trusted. Read and written with a fault caught, not
// with VirtualQuery first: this runs every frame, and on the game's memory
// that call took milliseconds (safe_memory.hpp).
float *client_angles(float now[2]) {
  uint8_t *data = client_data.load(std::memory_order_acquire);
  if (!data ||
      !xerces_safe::read(data + CLIENT_ANGLES, now, 2 * sizeof(float))) {
    return nullptr;
  }
  if (!std::isfinite(now[0]) || !std::isfinite(now[1]) ||
      std::abs(now[0]) > 360.0f || std::abs(now[1]) > 1.0e7f) {
    return nullptr;
  }
  return reinterpret_cast<float *>(data + CLIENT_ANGLES);
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

bool visible(int self, const vec3_t &eye, const vec3_t &point,
             const uint32_t mask = SIGHT_MASK) {
  game::cm::trace_t trace{};
  trace.fraction = 1.0f;
  int hit = self;
  game::cg::CG_SightTracePoint(&hit, &eye, &point,
                               static_cast<game::contents_t>(mask), &trace);
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
  uint32_t frame = 1; // never 0: a zombie never seen has frame 0
  float client_pitch = 0.0f; // the client's own angles as this left them
  float client_yaw = 0.0f;
  bool have_client = false;
  float hip_tan_x = 0.0f; // the widest view seen in this level: from the hip
  bool warned = false;
  int failed_frames = 0; // frames in a row the camera or the angles were not found
  int64_t last_pad_ms = 0;
  int64_t last_qpc = 0;
  // what the last frame did, for xerces_aim_debug and the test log
  int target = -1;
  float target_e = 0.0f;
  float target_metres = 0.0f;
  float strength = 0.0f;
  float slow = 0.0f;
  float settle = 0.0f; // degrees outside the leeway: up positive
  bool sights_up = false;
  // sums since the last test log line
  double sum_motion = 0.0;   // degrees the target moved across the view
  double sum_rotation = 0.0; // degrees the view was turned to follow
  int frames_in_bubble = 0;
  int64_t last_log_ms = 0;
};
state st;

int64_t now_ms() { return static_cast<int64_t>(GetTickCount64()); }

// Every way out of a frame comes through here, so what the zombies did
// meanwhile never counts as one frame's motion: the frame number jumps, and
// no track is "the frame before" any more (it used to be, and the first frame
// after a pause turned the view by the whole pause's motion)
void reset() {
  st.frame += 2;
  st.have_client = false;
  st.target = -1;
  st.strength = 0.0f;
  st.slow = 0.0f;
}

// Said once, and only when it lasts: the camera is away from the player for a
// moment whenever a level ends or the player is down and watching
void warn_once(const char *what) {
  if (st.warned || ++st.failed_frames < 2000) {
    return;
  }
  st.warned = true;
  xerces_runtime::log_warn("xerces.aim_assist.off",
                           std::string("Aim assist is off: ") + what +
                               " is not where this build of Xerces looks for "
                               "it (another game build, or a view that is not "
                               "the player's own)");
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
  vec3_t neck{};
  vec3_t head{};
  // the leeway, as elevations from the eye in degrees: the middle of the
  // torso and the top of the head
  float leeway_low = 0.0f;
  float leeway_high = 0.0f;
};

// How many of the nearest are tried for one the player sees
constexpr int CANDIDATES = 3;
struct candidates {
  candidate list[CANDIDATES];
  int count = 0;
};

// The live zombies nearest the middle of the bubble (up to half as far again
// out as its edge), nearest first, and every zombie's place this frame noted
// for the next
candidates find_targets(const camera &cam, const rules &r) {
  candidates found;
  const float view_elevation = -cam.pitch;
  const float cos_pitch = std::cos(cam.pitch / RAD);
  auto &entities = game::cg::cg_entitiesArray[0].entities;
  for (int i = 0; i < ENTITIES; ++i) {
    const auto &cent = entities[i];
    if (!cent.nextValid ||
        static_cast<uint8_t>(cent.pose.eType) != 0x12 /* ET_ACTOR */) {
      continue;
    }
    // dying or dead: no target, and no motion of its body is followed
    if ((cent.nextState.lerp.eFlags & ACTOR_DEAD) != 0 || cent.pose.isRagdoll ||
        cent.pose.ragdollHandle != 0) {
      st.tracks[i].frame = 0;
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
    const float neck_z = bottom + height * BODY_NECK;
    const float dz = neck_z - cam.eye.z;
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
    // and up and down nothing anywhere between its shins and the top of its
    // head (the box itself goes on 20 units over the head)
    const float ex = std::abs(normalize(yaw - cam.yaw)) * cos_pitch;
    const float low =
        std::atan2(bottom + height * BODY_BOTTOM - cam.eye.z, flat) * RAD;
    const float high =
        std::atan2(bottom + height * BODY_TOP - cam.eye.z, flat) * RAD;
    const float ev = view_elevation >= low && view_elevation <= high
                         ? 0.0f
                         : std::min(std::abs(view_elevation - low),
                                    std::abs(view_elevation - high));
    if (ex > 60.0f || ev > 60.0f) {
      continue;
    }
    const float sx = std::tan(ex / RAD) / cam.tan_x / r.bubble_x;
    const float sy = std::tan(ev / RAD) / cam.tan_y / r.bubble_y;
    const float e = std::sqrt(sx * sx + sy * sy);
    if (e > 1.5f) {
      continue;
    }
    // kept in order, nearest the middle first
    int place = found.count;
    while (place > 0 && found.list[place - 1].e > e) {
      --place;
    }
    if (place >= CANDIDATES) {
      continue;
    }
    for (int k = std::min(found.count, CANDIDATES - 1); k > place; --k) {
      found.list[k] = found.list[k - 1];
    }
    found.count = std::min(found.count + 1, CANDIDATES);
    candidate &c = found.list[place];
    c.index = i;
    c.e = e;
    c.dist = dist;
    c.yaw = yaw;
    c.pitch = pitch;
    c.had_previous = had_previous;
    c.previous_yaw = previous_yaw;
    c.previous_pitch = previous_pitch;
    c.neck = origin;
    c.neck.z = neck_z;
    c.head = origin;
    c.head.z = bottom + height * BODY_HEAD;
    c.leeway_low =
        std::atan2(bottom + height * BODY_TORSO - cam.eye.z, flat) * RAD;
    c.leeway_high =
        std::atan2(bottom + height * BODY_CROWN - cam.eye.z, flat) * RAD;
  }
  return found;
}

void test_log() {
  if (!test_pad || now_ms() - st.last_log_ms < log_every_ms) {
    return;
  }
  st.last_log_ms = now_ms();
  char text[256];
  std::snprintf(text, sizeof(text),
                "target %d at %.1f m, e %.2f, rotation %.2f, slowdown %.2f, "
                "%.1f deg outside the leeway, pitch %.2f; "
                "since the last line: %d frames in the bubble, target moved "
                "%.2f deg, view followed %.2f deg",
                st.target, st.target_metres, st.target_e, st.strength, st.slow,
                st.settle, st.client_pitch, st.frames_in_bubble, st.sum_motion,
                st.sum_rotation);
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

  // nothing is read, the controller included, outside a Zombies level
  if (!in_zombies_level()) {
    st.hip_tan_x = 0.0f;
    st.last_qpc = 0;
    reset();
    return;
  }
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
  float angles[2];
  float *view = client_angles(angles);
  if (!view) {
    warn_once("the client's view angles");
    reset();
    return;
  }

  st.failed_frames = 0;
  ++st.frame;
  // your own turn since this last left the view
  float own_pitch = 0.0f;
  float own_yaw = 0.0f;
  if (st.have_client) {
    own_pitch = normalize(angles[0] - st.client_pitch);
    own_yaw = normalize(angles[1] - st.client_yaw);
  }
  st.hip_tan_x = std::max(st.hip_tan_x, cam.tan_x);
  st.sights_up = cam.tan_x < st.hip_tan_x * SIGHTS_UP_FOV;

  float add_pitch = 0.0f;
  float add_yaw = 0.0f;
  st.target = -1;
  st.strength = 0.0f;
  st.slow = 0.0f;
  st.settle = 0.0f;
  // The nearest to the middle that the player sees (its neck or its head):
  // one behind a wall is no target, whatever stands nearer the crosshair
  candidate found;
  if (dt > 0.0f && dt < 0.25f) {
    const candidates closest = find_targets(cam, r);
    for (int k = 0; k < closest.count; ++k) {
      const candidate &c = closest.list[k];
      if (visible(client, cam.eye, c.neck) ||
          visible(client, cam.eye, c.head)) {
        found = c;
        break;
      }
    }
  } else {
    find_targets(cam, r); // the zombies' places, for the next frame
  }
  if (found.index >= 0) {
    const float metres = found.dist / UNITS_PER_METRE;
    const float in_zone = zone(found.e);
    float slow = r.slowdown * in_zone *
                 ramp(metres, r.range_slowdown, r.range_slowdown_count);
    if (!st.sights_up) {
      slow *= r.hip;
    }
    const bool input = pad.strafe >= STRAFE_MIN || pad.aim >= AIM_MIN;
    const float strength =
        input ? r.rotation * in_zone *
                    ramp(metres, r.range_rotation, r.range_rotation_count)
              : 0.0f;

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
    // The neck, with leeway: between the middle of the torso and the top of
    // the head the height is the player's own. Outside it, in the bubble and
    // with the stick in use, the view drifts to the nearer edge (the game's
    // pitch: down is positive).
    if (input && in_zone > 0.0f) {
      const float elevation = -cam.pitch;
      float off = 0.0f; // degrees to go: up positive
      if (elevation < found.leeway_low) {
        off = found.leeway_low - elevation;
      } else if (elevation > found.leeway_high) {
        off = found.leeway_high - elevation;
      }
      const float step =
          std::clamp(off * SETTLE_GAIN, -SETTLE_MAX, SETTLE_MAX) * dt * in_zone;
      add_pitch -= std::abs(step) > std::abs(off) ? off : step;
      st.settle = off;
    }
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
  if (add_pitch != 0.0f || add_yaw != 0.0f) {
    angles[0] += add_pitch;
    angles[1] += add_yaw;
    xerces_safe::write(view, angles, sizeof(angles));
  }
  st.client_pitch = angles[0];
  st.client_yaw = angles[1];
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
    float angles[2] = {0.0f, 0.0f};
    const float *view = client_angles(angles);
    std::snprintf(
        line, sizeof(line),
        "\ncamera %s: eye %.1f %.1f %.1f, pitch %.2f, yaw %.2f, "
        "tangents %.3f %.3f, sights %s\nclient angles %s: %.2f %.2f"
        "\ntarget %d at %.1f m, e %.2f, rotation %.2f, slowdown %.2f, "
        "%.1f deg outside the leeway",
        camera_ok ? "ok" : "NOT ok", cam.eye.x, cam.eye.y, cam.eye.z, cam.pitch,
        cam.yaw, cam.tan_x, cam.tan_y, st.sights_up ? "up" : "down",
        view ? "ok" : "NOT ok", angles[0], angles[1], st.target,
        st.target_metres, st.target_e, st.strength, st.slow, st.settle);
    text += line;
  }
  xerces_runtime::log_info("xerces.aim_assist.debug", text);
  printf("%s\n", text.c_str());
}
// Every actor within 40 m as the game has it right now, to the log: what tells
// a live zombie from a dying or dead one and a seen one from a hidden one was
// measured with this (2026-11-13). xerces_aim_actors [times]: that many
// lists, a quarter of a second apart.
void list_actors() {
  if (!in_zombies_level()) {
    xerces_runtime::log_info("xerces.aim_assist.actors", "not in a level");
    return;
  }
  camera cam;
  int client = 0;
  if (!read_camera(cam, client)) {
    xerces_runtime::log_info("xerces.aim_assist.actors", "no camera");
    return;
  }
  std::string text;
  int listed = 0;
  auto &entities = game::cg::cg_entitiesArray[0].entities;
  for (int i = 0; i < ENTITIES && listed < 24; ++i) {
    const auto &cent = entities[i];
    const auto type = static_cast<uint8_t>(cent.pose.eType);
    if (type < 0x12 || type > 0x14) {
      continue;
    }
    const vec3_t &origin = cent.pose.origin;
    const float dx = origin.x - cam.eye.x;
    const float dy = origin.y - cam.eye.y;
    const float dz = origin.z - cam.eye.z;
    const float metres =
        std::sqrt(dx * dx + dy * dy + dz * dz) / UNITS_PER_METRE;
    if (metres > 40.0f) {
      continue;
    }
    vec3_t chest = origin;
    chest.z += 57.0f; // the neck
    char line[320];
    std::snprintf(
        line, sizeof(line),
        "\n#%d type %02X valid %d %.1fm box %.0f..%.0f (%.0f wide) eye %.0f "
        "flags %08X %08X prev %08X "
        "solid %08X team %d ragdoll %d/%d bits %08X %08X %08X aimvis %d "
        "hidden %d rendered %d sight %d",
        i, type, cent.nextValid ? 1 : 0, metres, cent.pose.absmin.z - origin.z,
        cent.pose.absmax.z - origin.z, cent.pose.absmax.x - cent.pose.absmin.x,
        cam.eye.z - origin.z, static_cast<uint32_t>(cent.nextState.lerp.eFlags),
        static_cast<uint32_t>(cent.nextState.lerp.eFlags2),
        static_cast<uint32_t>(cent.prevState.eFlags),
        static_cast<uint32_t>(cent.nextState.solid),
        static_cast<int>(cent.nextState.team), cent.pose.isRagdoll ? 1 : 0,
        cent.pose.ragdollHandle != 0 ? 1 : 0, cent.packed_bits[0],
        cent.packed_bits[1], cent.packed_bits[2],
        cent.bAimTargetVisible ? 1 : 0, cent.bIsHiddenOnClient ? 1 : 0,
        cent.bRenderedLastFrame ? 1 : 0,
        visible(client, cam.eye, chest) ? 1 : 0);
    text += line;
    // the same with only solid blocking, as before 2026-11-13
    text += visible(client, cam.eye, chest, 1) ? " (solid only 1)"
                                               : " (solid only 0)";
    ++listed;
  }
  xerces_runtime::log_info("xerces.aim_assist.actors",
                           std::to_string(listed) + " actors" + text);
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
        "of Duty games' (slowdown and rotational). 0 the game's own, 1 "
        "modern, 2 modern and stronger.");
    load_xinput();
    wchar_t buffer[4];
    test_pad = GetEnvironmentVariableW(L"XERCES_AIM_TEST", buffer, 4) > 0 &&
               (buffer[0] == L'1' || buffer[0] == L'4');
    log_every_ms = test_pad && buffer[0] == L'4' ? 250 : 1000;
    hunk_alloc_hook.create(game::hunk::Hunk_UserAlloc.get(), hunk_alloc_stub);
    scheduler::loop(frame, scheduler::main);
    command::add("xerces_aim_debug", [] { debug(); });
    command::add("xerces_aim_actors", [](const command::params &params) {
      const int times =
          params.size() > 1 ? std::clamp(std::atoi(params.get(1)), 1, 80) : 1;
      const int every = params.size() > 2
                            ? std::clamp(std::atoi(params.get(2)), 20, 2000)
                            : 250;
      for (int i = 0; i < times; ++i) {
        scheduler::once(list_actors, scheduler::main,
                        std::chrono::milliseconds(every * i));
      }
    });
  }
};
} // namespace xerces_zm_aim_assist

REGISTER_COMPONENT(xerces_zm_aim_assist::component)
