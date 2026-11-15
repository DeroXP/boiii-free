#include <std_include.hpp>

// Client systems past the game's eight (docs/research/engine-re.md, "Client
// systems"; 2026-11-14 night). A client system is a named channel a level's
// server scripts use to tell its client scripts something:
// util::registerClientSys(name) calls the built-in ClientSysRegister, which
// writes "\n\<name>" into the first empty config string of 0xCAA to 0xCB1 and
// returns its index; util::setClientSysState calls ClientSysSetState(index,
// state), which sends the server command "0 <index> <that config string with
// \s\<state> set>" (spaces as '!'), and each client hands the state to the
// client script registered under the config string's name. ClientSysRegister
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
// unchanged. A friend's game without this client script misses the shared
// systems' states only (as it missed the left-out systems before).
//
// The built-ins are swapped in the game's own built-in tables (as
// character_slots.cpp does); the config string reader is the one
// ClientSysRegister calls (0x1421F4930, checked by its first bytes). The
// built-ins run on the server's script thread only, so the state here needs
// no lock.

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

using get_config_string_fn = void (*)(std::int32_t, char *, std::int32_t);
get_config_string_fn get_config_string = nullptr;

game::scr::builtin::BuiltinFunction register_orig = nullptr;
game::scr::builtin::BuiltinFunction set_state_orig = nullptr;
game::scr::builtin::BuiltinMethod set_state_method_orig = nullptr;

// This level's shared systems (server script thread only)
bool sharing = false;
std::string evicted;                     // the system that had the shared slot
std::vector<std::string> shared_systems; // ids first_virtual + i
bool warned_value = false;

bool same_name(const std::string_view a, const std::string_view b) {
  return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0;
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
    warned_value = false;
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

void send_shared(const std::int32_t client, const std::string &system,
                 const char *state) {
  std::string value = system + "|" + (state ? state : "");
  // As ClientSysSetState: no spaces in a config string's value
  std::ranges::replace(value, ' ', '!');
  // What the engine's Info_SetValueForKey refuses
  if (value.find_first_of("\\\";") != std::string::npos) {
    if (!warned_value) {
      warned_value = true;
      xerces_runtime::log_warn(
          "xerces.client_systems.value",
          "A shared client system's state has a character a config string "
          "cannot carry; not sent",
          std::format("{{\"system\":\"{}\"}}", system));
    }
    return;
  }
  const std::string command =
      std::format("0 {} \\n\\{}\\s\\{}", shared_slot, shared_name, value);
  game::sv::SV_GameSendServerCommand(static_cast<game::ClientNum_t>(client),
                                     game::net::RELIABLE, command.c_str());
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
  // Room left, or the name already has a slot: the engine's own
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
  if (entref && entref->classnum == 0 && entref->u.entnum >= 0 &&
      entref->u.entnum < max_clients) {
    if (const auto system = shared_system(game::scr::Scr_GetInt(inst, 0))) {
      send_shared(entref->u.entnum, *system, game::scr::Scr_GetString(inst, 1));
      return;
    }
  }
  set_state_method_orig(inst, entref);
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
    set_state_method_orig = player_methods->ClientSysSetState.actionFunc;
    player_methods->ClientSysSetState.actionFunc = ClientSysSetStateMethod;
  }
};
} // namespace xerces_client_systems

REGISTER_COMPONENT(xerces_client_systems::component)
