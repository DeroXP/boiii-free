#include <std_include.hpp>

// The Zombies character table after a map change (docs/research/engine-re.md,
// "The character table after a map change"; found 2026-11-14 night).
//
// BG_GetCharacterBodyType (0x1400ADD00) reads the session mode's character
// table from a cache of one entry per mode at 0x143EC4E40: {int mode, int
// state, const char *map, CustomizationTable *table}, 24 bytes. For Zombies
// the table belongs to the map: the entry is good while its map is the
// mapname dvar's value, and otherwise the loader (0x1400AD8E0) looks the
// table up again with DB_FindXAssetHeader, which gives an empty default
// table (no bodies) for one not loaded yet. A `map` from inside a level
// changes mapname before the new map's zones are in, and something asks for
// a body type at once: the entry then holds the new map's name with that
// empty table (seen from outside the process: Kino der Toten to Moon,
// `zm_moon`, table count 0), stays "good" for the whole level, and
// BG_GetCharacterBodyType returns null. Spawning the player
// (G_CharacterCustomization_SetupRenderOptionsFromClientStats_Internal,
// 0x1415DF0F0) reads the body type's count through it: a null read on the
// server thread, every time (Kino der Toten to Moon; Shadows of Evil to Moon
// was fine, the order of the lookups there differs). From the menus the
// lookup comes after the zones.
//
// So an empty Zombies table is not kept: after the loader, an entry whose
// table has no bodies gets a null table, which the cache's own check takes
// as "not loaded" (0x1400AD690), so the next use looks it up again, until
// the map's table is in. Nothing else changes: a table with bodies is kept
// as the engine keeps it, and other modes are left alone. The loader's
// first bytes and its reference to the cache are checked before the hook;
// anything else there is left alone and logged.

#include <game/game.hpp>
#include <loader/component_loader.hpp>

#include <utils/hook.hpp>

#include "runtime.hpp"

namespace xerces_character_table {
namespace {
constexpr std::uintptr_t table_loader = 0x1400AD8E0; // void (int mode)
constexpr std::uintptr_t table_cache = 0x143EC4E40;
// lea rcx, [rip + disp32] in the loader, the cache's address
constexpr std::uintptr_t cache_reference = 0x1400AD913;
constexpr int zombies = 0;

struct cache_entry {
  std::int32_t mode;
  std::int32_t state;
  const char *map;
  const std::uint8_t *table; // its body count is at +8
};
static_assert(sizeof(cache_entry) == 24);

utils::hook::detour loader_hook;
// Lookups that found the table empty since it was last found with bodies
std::atomic<std::uint32_t> empty_lookups{0};

bool loader_is_expected() {
  // mov [rsp+8], rbx; push rdi; sub rsp, 0x20; movsxd rdi, ecx
  static constexpr std::uint8_t prologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08,
                                              0x57, 0x48, 0x83, 0xEC, 0x20,
                                              0x48, 0x63, 0xF9};
  const auto *code =
      reinterpret_cast<const std::uint8_t *>(game::relocate(table_loader));
  if (std::memcmp(code, prologue, sizeof(prologue)) != 0) {
    return false;
  }
  const auto *lea =
      reinterpret_cast<const std::uint8_t *>(game::relocate(cache_reference));
  if (lea[0] != 0x48 || lea[1] != 0x8D || lea[2] != 0x0D) {
    return false;
  }
  std::int32_t displacement = 0;
  std::memcpy(&displacement, lea + 3, sizeof(displacement));
  const std::uintptr_t target =
      game::relocate(cache_reference) + 7 + displacement;
  return target == game::relocate(table_cache);
}

void loader_stub(const int mode) {
  loader_hook.invoke<void>(mode);
  if (mode != zombies) {
    return;
  }
  auto *entry = reinterpret_cast<cache_entry *>(game::relocate(table_cache));
  const std::uint8_t *table = entry->table;
  if (!table) {
    return;
  }
  std::uint32_t bodies = 0;
  std::memcpy(&bodies, table + 8, sizeof(bodies));
  if (bodies == 0) {
    entry->table = nullptr; // looked up again at its next use
    if (empty_lookups.fetch_add(1) == 0) {
      xerces_runtime::log_info(
          "xerces.character_table.empty",
          std::format("The Zombies character table for {} is not loaded yet; "
                      "it is looked up again at its next use",
                      entry->map ? entry->map : "?"));
    }
    return;
  }
  if (const std::uint32_t empty = empty_lookups.exchange(0); empty != 0) {
    xerces_runtime::log_info(
        "xerces.character_table.found",
        std::format("The Zombies character table for {} is in ({} bodies) "
                    "after {} empty lookup(s)",
                    entry->map ? entry->map : "?", bodies, empty));
  }
}
} // namespace

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_character_table");

public:
  void post_unpack() override {
    if (!game::is_new_client()) {
      return;
    }
    if (!loader_is_expected()) {
      xerces_runtime::log_warn("xerces.character_table.off",
                               "The character table's loader is not the "
                               "expected code; left as it is");
      return;
    }
    try {
      loader_hook.create(reinterpret_cast<void *>(game::relocate(table_loader)),
                         reinterpret_cast<void *>(&loader_stub));
    } catch (const std::exception &e) {
      loader_hook.clear();
      xerces_runtime::log_warn(
          "xerces.character_table.off",
          std::string("The character table's loader could not be hooked (") +
              e.what() + ")");
    }
  }
};
} // namespace xerces_character_table

REGISTER_COMPONENT(xerces_character_table::component)
