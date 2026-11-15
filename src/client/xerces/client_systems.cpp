#include <std_include.hpp>

// Client systems past the game's eight (docs/research/engine-re.md, "Client
// systems"; 2026-11-14 night). A client system is a named channel a level's
// server scripts use to tell its client scripts something:
// util::registerClientSys(name) calls the built-in ClientSysRegister, which
// writes "\n\<name>" into the first empty config string of 0xCAA to 0xCB1 and
// returns its index; util::setClientSysState calls ClientSysSetState(index,
// state), which sends the server command "0 <index> \s\<state>" (spaces as
// '!'; the info string's '\', ';' and '"' left out), and each client hands
// the state to the client script registered under its copy of that slot's
// config string's name. ClientSysRegister
// has eight slots (0 to 7); a ninth system gets -1, and the first state set on
// it ends the match ("state index (-1) out of bounds"). A map and a stack of
// mods need more: Kino der Toten four, DS4C three, two more from other mods.
// Until now Xerces' script took such a system out of the list, so its client
// side never heard from it (Kino's mystery box lights stayed dark).
//
// Now none is left out. While there is room the engine's own slots are used
// as always. When a system finds them full, slot 7 becomes the shared one
// ("xerces_mux"): the system that had it and every later one get their states
// sent through it as "<name>|<state>", and Xerces' client script
// (data/custom_scripts/zm/xerces_client_systems.csc) hands each to the client
// script registered under that name. Later systems get ids from 64 up, which
// only these built-ins read. A level with eight or fewer systems is
// unchanged. A friend whose game lacks this client script (an older Xerces,
// or none) misses the states of the shared ones, the one moved out of slot 7
// included (on Kino with eight mods: lsm, the map's lights, as well as
// box_indicator, the one left out before).
//
// Zombies only (the client script is there), and only while Xerces' custom
// client scripts are linked (script::client_scripts_hooked): otherwise the
// engine's own built-ins, as before.
//
// The player method is never handed back to the engine's: its code is
// protected and stops in a loop when called from outside the game's image
// (seen 2026-11-15 as a frozen server on Ascension with eight mods, where
// Girls' Frontline sends a slot's state to a player as they connect). So the
// method is Xerces' for every id, doing what the engine's does: the same
// checks and script errors, and the same command to that player.
//
// The built-ins are swapped in the game's own built-in tables (as
// character_slots.cpp does); the config string reader is the one
// ClientSysRegister calls (0x1421F4930, checked by its first bytes). The
// built-ins run on the server's script thread only, so the state here needs
// no lock.

#include <component/script.hpp>
#include <game/game.hpp>
#include <loader/component_loader.hpp>

#include "runtime.hpp"

namespace xerces_client_systems {
namespace {
constexpr std::int32_t first_config_string = 0xCAA;
constexpr std::int32_t engine_slots = 8; // ClientSysRegister: 0 to 7
constexpr std::int32_t shared_slot = engine_slots - 1;
constexpr std::int32_t first_virtual = 64;
constexpr std::string_view shared_name = "xerces_mux";
// SV_GetConfigstring(index, buffer, size), new client
constexpr std::uintptr_t get_config_string_address = 0x1421F4930;
constexpr std::size_t config_string_size = 0x400;
// The engine's client numbers (character_slots.cpp keeps to the same bound)
constexpr std::int32_t max_clients = 32;
// ClientSysSetState takes 0 to 8 (one more than registration hands out)
constexpr std::int32_t engine_last_index = 8;

using get_config_string_fn = void (*)(std::int32_t, char *, std::int32_t);
get_config_string_fn get_config_string = nullptr;

game::scr::builtin::BuiltinFunction register_orig = nullptr;
game::scr::builtin::BuiltinFunction set_state_orig = nullptr;

// This level's shared systems (server script thread only)
bool sharing = false;
std::string evicted;                     // the system that had the shared slot
std::vector<std::string> shared_systems; // ids first_virtual + i

bool same_name(const std::string_view a, const std::string_view b) {
  return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0;
}

// The slot's config string is empty (the engine's "unregistered" test)
bool slot_empty(const std::int32_t slot) {
  std::array<char, config_string_size> buffer{};
  get_config_string(first_config_string + slot, buffer.data(),
                    static_cast<std::int32_t>(buffer.size()));
  return buffer[0] == '\0';
}

// The name in a slot's config string, empty when the slot is free
std::string name_in_slot(const std::int32_t slot) {
  std::array<char, config_string_size> buffer{};
  get_config_string(first_config_string + slot, buffer.data(),
                    static_cast<std::int32_t>(buffer.size()));
  if (buffer[0] == '\0') {
    return {};
  }
  const char *name = game::info::Info_ValueForKey(buffer.data(), "n");
  return name ? std::string(name) : std::string();
}

// A new level clears the config strings: the shared slot is gone with them
void check_level() {
  if (sharing && !same_name(name_in_slot(shared_slot), shared_name)) {
    sharing = false;
    evicted.clear();
    shared_systems.clear();
  }
}

// The system a shared id stands for, if it is one
std::optional<std::string> shared_system(const std::int32_t id) {
  check_level();
  if (!sharing) {
    return std::nullopt;
  }
  if (id == shared_slot) {
    return evicted;
  }
  if (id >= first_virtual &&
      id - first_virtual < static_cast<std::int32_t>(shared_systems.size())) {
    return shared_systems[id - first_virtual];
  }
  return std::nullopt;
}

// What ClientSysSetState sends: "0 <index> \s\<state>", the state cut to
// 1023 characters (its 0x400 buffer), spaces as '!', the characters an info
// string's value cannot hold left out as the engine's Info_SetValueForKey
// leaves them out, and nothing at all when the info string would pass 1024
// characters (Info_SetValueForKey refuses it and the engine sends "0 <index> ")
void send_state(const std::int32_t client, const std::int32_t index,
                const std::string_view state) {
  constexpr std::size_t info_string_max = 1024;
  const std::string_view cut = state.substr(0, info_string_max - 1);
  std::string value;
  value.reserve(cut.size());
  for (const char c : cut) {
    if (c == '\\' || c == ';' || c == '"') {
      continue;
    }
    value.push_back(c == ' ' ? '!' : c);
  }
  const std::string info = std::format("\\s\\{}", value);
  const std::string command =
      std::format("0 {} {}", index, info.size() < info_string_max ? info : "");
  game::sv::SV_GameSendServerCommand(static_cast<game::ClientNum_t>(client),
                                     game::net::RELIABLE, command.c_str());
}

void send_shared(const std::int32_t client, const std::string &system,
                 const char *state) {
  send_state(client, shared_slot, system + "|" + (state ? state : ""));
}

// The engine's checks and script errors, for the player method
bool index_ok(const game::scr::scriptInstance_t inst, const std::int32_t id) {
  if (id < 0 || id > engine_last_index) {
    game::scr::Scr_Error(
        inst,
        std::format("ClientSysSetState - state index ({}) out of bounds (0 - "
                    "{})",
                    id, engine_last_index)
            .c_str(),
        true);
    return false;
  }
  if (slot_empty(id)) {
    game::scr::Scr_Error(
        inst,
        std::format("ClientSysSetState - state index ({}) unregistered.  Use "
                    "ClientSysRegister first.",
                    id)
            .c_str(),
        true);
    return false;
  }
  return true;
}

// A player's entity (its number is the player's client number), as the
// engine's player methods require
bool is_player(const game::scr::scr_entref_t *entref) {
  if (!entref || entref->classnum != 0 || entref->u.entnum < 0 ||
      entref->u.entnum >= max_clients) {
    return false;
  }
  const game::level::gentity_pool *pool = game::level::get_g_entities();
  if (!pool) {
    return false;
  }
  const game::level::gentity_t &ent = pool->pool[entref->u.entnum];
  return ent.r.inuse && ent.client != nullptr;
}

// Sharing is set up (Zombies, Xerces' client scripts linked)
bool sharing_possible() {
  return game::com::Com_SessionMode_IsMode(game::eModes::ZOMBIES) &&
         script::client_scripts_hooked();
}

void ClientSysRegister(const game::scr::scriptInstance_t inst) {
  const char *raw = game::scr::Scr_GetString(inst, 0);
  if (!raw) {
    register_orig(inst);
    return;
  }
  const std::string name = raw;
  check_level();
  if (sharing) {
    if (same_name(name, evicted)) {
      game::scr::Scr_AddInt(inst, shared_slot);
      return;
    }
    for (std::size_t i = 0; i < shared_systems.size(); ++i) {
      if (same_name(name, shared_systems[i])) {
        game::scr::Scr_AddInt(inst,
                              first_virtual + static_cast<std::int32_t>(i));
        return;
      }
    }
  }
  // Room left, or the name already has a slot, or no sharing here: the
  // engine's own
  if (!sharing_possible()) {
    register_orig(inst);
    return;
  }
  bool room = false;
  for (std::int32_t slot = 0; slot < engine_slots; ++slot) {
    const std::string in_slot = name_in_slot(slot);
    if (in_slot.empty()) {
      room = true;
    } else if (same_name(in_slot, name)) {
      register_orig(inst);
      return;
    }
  }
  if (room) {
    register_orig(inst);
    return;
  }
  if (!sharing) {
    evicted = name_in_slot(shared_slot);
    const std::string infostring = std::format("\\n\\{}", shared_name);
    game::sv::SV_SetConfigString(first_config_string + shared_slot,
                                 infostring.c_str());
    sharing = true;
    xerces_runtime::log_info(
        "xerces.client_systems.shared",
        std::format("The game's eight client systems are taken: {} and the "
                    "ones after it share slot {}",
                    evicted, shared_slot));
  }
  shared_systems.push_back(name);
  xerces_runtime::log_info(
      "xerces.client_systems.added",
      std::format("Client system {} shares slot {} ({} sharing it)", name,
                  shared_slot, shared_systems.size() + 1));
  game::scr::Scr_AddInt(
      inst,
      first_virtual + static_cast<std::int32_t>(shared_systems.size()) - 1);
}

void ClientSysSetState(const game::scr::scriptInstance_t inst) {
  if (const auto system = shared_system(game::scr::Scr_GetInt(inst, 0))) {
    send_shared(-1, *system, game::scr::Scr_GetString(inst, 1));
    return;
  }
  set_state_orig(inst);
}

void ClientSysSetStateMethod(const game::scr::scriptInstance_t inst,
                             game::scr::scr_entref_t *entref) {
  if (!is_player(entref)) {
    // Scr_ObjectError in the engine: not a terminal error (a review,
    // 2026-11-15)
    game::scr::Scr_Error(
        inst,
        std::format("entity {} is not a player", entref ? entref->u.entnum : -1)
            .c_str(),
        false);
    return;
  }
  const std::int32_t id = game::scr::Scr_GetInt(inst, 0);
  if (const auto system = shared_system(id)) {
    send_shared(entref->u.entnum, *system, game::scr::Scr_GetString(inst, 1));
    return;
  }
  if (!index_ok(inst, id)) {
    return;
  }
  // The state after the checks, as the engine reads it
  const char *state = game::scr::Scr_GetString(inst, 1);
  send_state(entref->u.entnum, id, state ? state : "");
}

bool reader_is_expected() {
  // mov [rsp+8], rbx; mov [rsp+0x10], rsi; push rdi; sub rsp, 0x30
  static constexpr std::uint8_t start[] = {0x48, 0x89, 0x5C, 0x24, 0x08,
                                           0x48, 0x89, 0x74, 0x24, 0x10,
                                           0x57, 0x48, 0x83, 0xEC, 0x30};
  const auto *code = reinterpret_cast<const std::uint8_t *>(
      game::relocate(get_config_string_address));
  return std::memcmp(code, start, sizeof(start)) == 0;
}
} // namespace

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_client_systems");

public:
  void post_unpack() override {
    if (!game::is_new_client()) {
      return;
    }
    if (!reader_is_expected()) {
      xerces_runtime::log_warn(
          "xerces.client_systems.off",
          "The config string reader is not the expected code; client "
          "systems past eight are left out as before");
      return;
    }
    get_config_string = reinterpret_cast<get_config_string_fn>(
        game::relocate(get_config_string_address));
    using namespace game::scr::builtin::table;
    register_orig = gscr::builtin_functions->ClientSysRegister.actionFunc;
    gscr::builtin_functions->ClientSysRegister.actionFunc = ClientSysRegister;
    set_state_orig = gscr::builtin_functions->ClientSysSetState.actionFunc;
    gscr::builtin_functions->ClientSysSetState.actionFunc = ClientSysSetState;
    player_methods->ClientSysSetState.actionFunc = ClientSysSetStateMethod;
  }
};
} // namespace xerces_client_systems

REGISTER_COMPONENT(xerces_client_systems::component)
