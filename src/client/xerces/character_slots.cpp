#include <std_include.hpp>

// Zombies character slots (docs/DESIGN.md 7.10 and 7.12).
//
// Priority: a player who picked a character in Xerces' CHARACTERS menu keeps it.
// Another mod's own character script (DS4C's) calls SetCharacterBodyType,
// SetCharacterBodyStyle and SetCharacterHelmetStyle on the player, at spawn and
// when its own menu is used. While the player's bit is set in the xerces_char_lock
// dvar, those calls are skipped unless xerces_char_apply is 1, which Xerces' own
// script (data/custom_scripts/zm/xerces_characters.gsc) holds while it makes the
// same calls (no wait in between, so no other script runs meanwhile).
//
// Looks: every character mod's characters as slots of their own. The game has
// one character table per match (zm_character_customization, or its _zmhd
// twin): up to 16 characters ("bodies"), each with up to 16 outfits, and an
// outfit is a list of models (body, first-person hands, legs, ...). Character
// mods come in two kinds, and with several loaded only the first of each got
// seen:
// - a mod with its own table (DS4C, Girls' Frontline, Avali): the game uses the
//   table of the mod highest in the stack, the others' are loaded and unused;
// - a mod that replaces the base characters' models by name (DBZ Villain
//   Characters, Hyperdimension Neptunia; the table mods do this too): the model
//   of the mod highest in the stack is the one every table points at.
// The game keeps every replaced copy of a table and of a model loaded, each
// with its own meshes and materials (mod_stack::asset_copies; seen in the game
// on 2026-11-12). So for every mod below the one in use, each of its
// characters is put together from its own copies (a model it has no copy of
// comes from the game) and added to the table in use as one more outfit of the
// character with the same index, or of the one with the most room. The pick is
// then an ordinary (character, outfit) pair: the game sends it to every
// player, and each player's Xerces adds the same outfits in the same order,
// because the mod stack is the same for all of them. Nothing is hooked: the
// table's outfit list and count are pointed at a list of Xerces' own while a
// Zombies match runs, and back when it ends.
//
// Trace: with XERCES_CHAR_TRACE=1 in the environment, every script call that
// changes what a player wears (SetModel, Attach, DetachAll, SetPlayerCharacter,
// SetCharacterBodyType, SetCharacterBodyStyle) is written to the game log with
// the player and its first argument, so a mod's own character script can be told
// apart from Xerces'. At most 600 lines of the character calls and 600 of the
// rest. Off otherwise.

#include <component/command.hpp>
#include <component/mod_stack.hpp>
#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <game/impl/scr/scr.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>

#include "character_slots.hpp"
#include "runtime.hpp"
#include "safe_memory.hpp"

using namespace game;
using namespace game::scr;
using game::scr::builtin::BuiltinMethod;

namespace xerces_character_slots {
namespace {
// The character calls and the rest (SetModel, Attach: every zombie makes
// them) are counted apart, so the rest cannot use the character calls' lines up
std::atomic<int> logged{0};
std::atomic<int> logged_other{0};
constexpr int max_lines = 600;

BuiltinMethod set_model_orig = nullptr;
BuiltinMethod attach_orig = nullptr;
BuiltinMethod detach_all_orig = nullptr;
BuiltinMethod set_player_character_orig = nullptr;
BuiltinMethod set_body_type_orig = nullptr;
BuiltinMethod set_body_style_orig = nullptr;

void note(const char *what, scriptInstance_t inst, scr_entref_t *entref,
          bool text) {
  std::atomic<int> &count =
      std::strncmp(what, "SetCharacter", 12) == 0 ? logged : logged_other;
  if (count.fetch_add(1) >= max_lines) {
    return;
  }
  std::string arg = "-";
  if (Scr_GetNumParam(inst) > 0) {
    arg = text ? std::string(Scr_GetString(inst, 0))
               : std::to_string(Scr_GetInt(inst, 0));
  }
  xerces_runtime::log_info(
      "char.call", std::string(what) + " " + arg,
      "{\"call\":\"" + std::string(what) + "\",\"class\":" +
          std::to_string(entref->classnum) + ",\"ent\":" +
          std::to_string(entref->u.entnum) + ",\"arg\":\"" + arg + "\"}");
}

#define WRAP(name, orig, is_text)                                              \
  void name(scriptInstance_t inst, scr_entref_t *entref) {                     \
    note(#name, inst, entref, is_text);                                        \
    orig(inst, entref);                                                        \
  }

WRAP(SetModel, set_model_orig, true)
WRAP(Attach, attach_orig, true)
WRAP(DetachAll, detach_all_orig, false)
WRAP(SetPlayerCharacter, set_player_character_orig, false)

bool tracing = false;
BuiltinMethod set_helmet_style_orig = nullptr;

// Whether a call from another script on this player is to be skipped
bool locked(scr_entref_t *entref) {
  if (entref->classnum != 0 || entref->u.entnum > 31) {
    return false;
  }
  const auto lock = game::get_dvar_int("xerces_char_lock");
  if (!lock || !((*lock >> entref->u.entnum) & 1)) {
    return false;
  }
  const auto applying = game::get_dvar_int("xerces_char_apply");
  return !applying || *applying != 1;
}

// ------------------------------------------------------------------ looks
// The game's own layouts, measured in the game on 2026-11-12 (the structs in
// game/structs are not exact here): a table is its name, a count and its
// bodies in a row; a body is 0x1C0 bytes, an outfit 0xA8.
constexpr size_t BODY_SIZE = 0x1C0;
constexpr size_t BODY_NAME = 0x00;
constexpr size_t BODY_DISPLAY_NAME = 0x08;
constexpr size_t BODY_ICON = 0x40;
constexpr size_t BODY_NUM_STYLES = 0xA8;
constexpr size_t BODY_STYLES = 0xB0;
constexpr size_t ITEM_SIZE = 0xA8;
constexpr size_t ITEM_NAME = 0x00;
constexpr size_t ITEM_MODELS = 0x18;
constexpr int ITEM_MODEL_COUNT = 11;
// A character's body has 100 bones and more, its first-person hands 60
constexpr int MIN_BONES = 30;
// What a player's character is sent as: 4 bits each
constexpr uint32_t MAX_BODIES = 16;
constexpr uint32_t MAX_STYLES = 16;
constexpr const char *TABLE_NAMES[] = {"zm_character_customization",
                                       "zm_character_customization_zmhd"};

using item_t = std::array<uint8_t, ITEM_SIZE>;

// A string at this address, if it reads as one
std::string text_at(const void *address) {
  char buffer[96];
  const size_t length = xerces_safe::read_text(address, buffer, sizeof(buffer));
  return std::string(buffer, length);
}

template <typename T> T field(const void *base, size_t offset) {
  T value;
  std::memcpy(&value, static_cast<const uint8_t *>(base) + offset, sizeof(T));
  return value;
}

template <typename T> void put(void *base, size_t offset, T value) {
  std::memcpy(static_cast<uint8_t *>(base) + offset, &value, sizeof(T));
}

// The game's own memory: read into a value, false if it is gone
template <typename T> bool peek(const void *address, T &out) {
  return xerces_safe::read(address, &out, sizeof(T));
}

template <typename T> bool poke(void *address, T value) {
  return xerces_safe::write(address, &value, sizeof(T));
}

struct look {
  std::string source; // a Workshop id; "" the game's own
  std::string name;
  std::string hint; // what the body's model is made from
  std::string icon;
  uint32_t body = 0; // the slot: character and outfit in the table in use
  uint32_t style = 0;
  uint32_t id = 0;
  bool added = false; // an outfit Xerces added
  // the mod has no table of its own: the name is the character whose models
  // it replaces, and the hint says who it is
  bool in_place = false;
  std::string made_of; // its mesh and first materials, to tell two apart
};

// One body of a table in use: what its outfit list was, and Xerces' own
struct record {
  const uint8_t *original_items = nullptr;
  uint32_t original_count = 0;
  std::unique_ptr<uint8_t[]> buffer;
};

std::mutex looks_mutex;
std::vector<look> looks;
std::unordered_map<uint8_t *, record> records;
uint64_t built_links = UINT64_MAX;
const void *built_table = nullptr;
int dropped_looks = 0;
double build_ms = 0.0;

std::string plain(std::string text) {
  for (char &c : text) {
    if (c == '\t' || c == '\n' || c == '\r') {
      c = ' ';
    }
  }
  return text;
}

// A mesh's name without the number the game's tools end it with
std::string clean(std::string name) {
  size_t cut = 0;
  while (cut < 8 && cut < name.size() &&
         std::isxdigit(static_cast<unsigned char>(name[name.size() - 1 - cut]))) {
    ++cut;
  }
  if (cut >= 6 && name.size() > cut) {
    name.resize(name.size() - cut);
  }
  return plain(std::move(name));
}

uint32_t look_id(const std::string &text) {
  uint32_t hash = 2166136261u;
  for (const char c : text) {
    hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
  }
  hash &= 0x7FFFFFFF;
  return hash ? hash : 1;
}

// The table in use gets its own outfit lists back (a match ended, or nothing
// to add). A body that is gone, or was loaded again, no longer points at
// Xerces' list and is left alone.
void restore_all() {
  for (auto &[body, rec] : records) {
    const uint8_t *now = nullptr;
    if (!rec.buffer || !peek(body + BODY_STYLES, now) ||
        now != rec.buffer.get()) {
      continue;
    }
    poke(body + BODY_NUM_STYLES, rec.original_count);
    poke(body + BODY_STYLES, rec.original_items);
  }
}

// The stack's mods from the top, then the game
std::vector<std::string> source_order() {
  std::vector<std::string> order;
  const std::string main = mod_stack::main_id();
  if (!main.empty()) {
    order.push_back(main);
  }
  for (std::string &id : mod_stack::get()) {
    order.push_back(std::move(id));
  }
  order.emplace_back();
  return order;
}

// A body of a table, copied out of the game's memory with its outfits
struct body_copy {
  std::array<uint8_t, BODY_SIZE> bytes{};
  std::vector<item_t> items;
  bool ok = false;
};

struct table_copy {
  std::string owner;
  std::vector<body_copy> bodies;
};

bool read_items(const uint8_t *items, uint32_t count, std::vector<item_t> &out) {
  if (count == 0 || count > MAX_STYLES) {
    return false;
  }
  out.resize(count);
  for (uint32_t s = 0; s < count; ++s) {
    if (!xerces_safe::read(items + s * ITEM_SIZE, out[s].data(), ITEM_SIZE)) {
      return false;
    }
  }
  return true;
}

// Works out every look and points the table in use at the outfits added.
// False when the table in use is not one of the tables' copies (yet).
bool build(const uint8_t *in_use) {
  using namespace game::db::xasset;
  const auto started = std::chrono::steady_clock::now();
  const auto xmodel_type = static_cast<int32_t>(XAssetType::XMODEL);
  std::vector<look> found;
  int dropped = 0;

  std::unordered_map<std::string, std::vector<mod_stack::asset_copy>> copies;
  const auto copies_of =
      [&](const std::string &name) -> const std::vector<mod_stack::asset_copy> & {
    const auto [entry, added] = copies.try_emplace(name);
    if (added) {
      entry->second = mod_stack::asset_copies(xmodel_type, name);
    }
    return entry->second;
  };
  std::unordered_map<const void *, std::string> names;
  const auto model_name = [&](const void *model) -> const std::string & {
    const auto [entry, added] = names.try_emplace(model);
    const char *name = nullptr;
    if (added && (reinterpret_cast<uintptr_t>(model) & 7) == 0 &&
        peek(model, name)) {
      entry->second = text_at(name);
    }
    return entry->second;
  };

  // the table in use first, then the copies of it that lost
  std::vector<table_copy> tables;
  for (const char *table_name : TABLE_NAMES) {
    const auto table_copies = mod_stack::asset_copies(
        static_cast<int32_t>(XAssetType::CUSTOMIZATION_TABLE), table_name);
    if (table_copies.empty()) {
      continue;
    }
    CustomizationTable live{};
    if (!peek(table_copies[0].header, live) ||
        reinterpret_cast<const uint8_t *>(live.playerRoles) != in_use) {
      continue;
    }
    for (size_t i = 0; i < table_copies.size(); ++i) {
      const mod_stack::asset_copy &copy = table_copies[i];
      // a custom map's own copy goes when the map does
      if (i > 0 && copy.from_map) {
        continue;
      }
      CustomizationTable table{};
      if (!peek(copy.header, table)) {
        continue;
      }
      table_copy out;
      out.owner = copy.from_map ? std::string("map") : copy.owner;
      const uint32_t count =
          std::min<uint32_t>(table.numPlayerRoles, i == 0 ? MAX_BODIES : 32);
      const auto *bodies = reinterpret_cast<const uint8_t *>(table.playerRoles);
      out.bodies.resize(count);
      bool any = false;
      for (uint32_t b = 0; b < count; ++b) {
        body_copy &body = out.bodies[b];
        if (!xerces_safe::read(bodies + b * BODY_SIZE, body.bytes.data(),
                               BODY_SIZE)) {
          break;
        }
        if (i > 0) {
          body.ok = read_items(field<const uint8_t *>(body.bytes.data(),
                                                      BODY_STYLES),
                               field<uint32_t>(body.bytes.data(),
                                               BODY_NUM_STYLES),
                               body.items);
        }
        any = true;
      }
      if (!any && i == 0) {
        break;
      }
      tables.push_back(std::move(out));
    }
    break;
  }
  if (tables.empty()) {
    return false;
  }

  // each body of the table in use: its own outfits, then the ones added
  struct slots_t {
    uint8_t *body = nullptr;
    record *rec = nullptr;
    std::vector<item_t> items;
    uint32_t own = 0;
    bool usable = false;
  };
  const auto bodies = static_cast<uint32_t>(tables[0].bodies.size());
  std::vector<slots_t> slots(bodies);
  for (uint32_t b = 0; b < bodies; ++b) {
    slots_t &slot = slots[b];
    body_copy &copy = tables[0].bodies[b];
    slot.body = const_cast<uint8_t *>(in_use) + b * BODY_SIZE;
    record &rec = records[slot.body];
    slot.rec = &rec;
    const auto *items = field<const uint8_t *>(copy.bytes.data(), BODY_STYLES);
    auto count = field<uint32_t>(copy.bytes.data(), BODY_NUM_STYLES);
    if (rec.buffer && items == rec.buffer.get()) {
      items = rec.original_items;
      count = rec.original_count;
    } else {
      rec.original_items = items;
      rec.original_count = count;
    }
    if (!rec.buffer) {
      rec.buffer = std::make_unique<uint8_t[]>(MAX_STYLES * ITEM_SIZE);
    }
    if (!read_items(items, count, slot.items)) {
      slot.items.clear();
      continue;
    }
    slot.usable = true;
    slot.own = count;
    copy.items = slot.items;
    copy.ok = true;
  }

  const std::string live_owner = tables[0].owner;
  for (const std::string &source : source_order()) {
    // its own table if it has one; else the table in use and the game's, with
    // its models in them
    std::vector<size_t> templates;
    bool own_table = false;
    for (size_t t = 0; t < tables.size(); ++t) {
      if (tables[t].owner == source) {
        templates = {t};
        own_table = true;
        break;
      }
    }
    if (!own_table) {
      templates.push_back(0);
      for (size_t t = 1; t < tables.size(); ++t) {
        if (tables[t].owner.empty()) {
          templates.push_back(t);
        }
      }
    }
    for (const size_t t : templates) {
      const table_copy &table = tables[t];
      for (uint32_t i = 0; i < table.bodies.size(); ++i) {
        const body_copy &body = table.bodies[i];
        if (!body.ok) {
          continue;
        }
        for (uint32_t j = 0; j < body.items.size(); ++j) {
          item_t item = body.items[j];
          bool owned = false;
          bool body_owned = false;
          const void *body_model = nullptr;
          for (int m = 0; m < ITEM_MODEL_COUNT; ++m) {
            const size_t at = ITEM_MODELS + static_cast<size_t>(m) * 8;
            const void *model = field<const void *>(item.data(), at);
            const std::string &name = model_name(model);
            if (name.empty()) {
              continue;
            }
            const mod_stack::asset_copy *mine = nullptr;
            const mod_stack::asset_copy *stock = nullptr;
            for (const mod_stack::asset_copy &copy : copies_of(name)) {
              if (copy.from_map) {
                continue;
              }
              if (!mine && copy.owner == source) {
                mine = &copy;
              }
              if (!stock && copy.owner.empty()) {
                stock = &copy;
              }
            }
            const mod_stack::asset_copy *use = mine ? mine : stock;
            if (use) {
              put(item.data(), at, use->header);
            }
            if (mine) {
              owned = true;
              body_owned = body_owned || m == 0;
            }
            if (m == 0) {
              body_model = use ? use->header : model;
            }
          }
          // a mod's character has a model of the mod's; the game's are all its
          // own
          if (!source.empty() && (own_table ? !owned : !body_owned)) {
            continue;
          }
          // an outfit whose body is no character (Girls' Frontline has two
          // made of an effect's model) would leave the player unseen
          xmodel::XModel model{};
          if (!peek(body_model, model) || model.numBones < MIN_BONES) {
            continue;
          }

          const auto same_models = [&](const item_t &other) {
            return std::memcmp(other.data() + ITEM_MODELS,
                               item.data() + ITEM_MODELS,
                               ITEM_MODEL_COUNT * 8) == 0;
          };
          int at_body = -1;
          int at_style = -1;
          const auto find_in = [&](uint32_t b) {
            for (size_t s = 0; at_body < 0 && s < slots[b].items.size(); ++s) {
              if (same_models(slots[b].items[s])) {
                at_body = static_cast<int>(b);
                at_style = static_cast<int>(s);
              }
            }
          };
          if (i < bodies) {
            find_in(i);
          }
          for (uint32_t b = 0; at_body < 0 && b < bodies; ++b) {
            if (b != i) {
              find_in(b);
            }
          }
          bool added = false;
          if (at_body >= 0) {
            added = static_cast<uint32_t>(at_style) >= slots[at_body].own;
          } else {
            // the character with the same index, else the one with most room
            uint32_t to = bodies;
            if (i < bodies && slots[i].usable &&
                slots[i].items.size() < MAX_STYLES) {
              to = i;
            } else {
              for (uint32_t b = 0; b < bodies; ++b) {
                if (slots[b].usable && slots[b].items.size() < MAX_STYLES &&
                    (to == bodies ||
                     slots[b].items.size() < slots[to].items.size())) {
                  to = b;
                }
              }
            }
            if (to == bodies) {
              ++dropped;
              continue;
            }
            slots[to].items.push_back(item);
            at_body = static_cast<int>(to);
            at_style = static_cast<int>(slots[to].items.size() - 1);
            added = true;
          }
          // the table in use lists its own characters already
          if (!added && source == live_owner) {
            continue;
          }
          // one row for a slot: a mod that gives two characters the same
          // models (Avali's nine are four characters) has it once
          if (std::ranges::any_of(found, [&](const look &one) {
                return one.source == source &&
                       one.body == static_cast<uint32_t>(at_body) &&
                       one.style == static_cast<uint32_t>(at_style);
              })) {
            continue;
          }

          // what the body is drawn from: two outfits of one name drawn from
          // the same are one character (a mod puts its Dempsey on each of the
          // game's Dempsey models)
          std::string made_of = std::format(
              "{:p}", static_cast<const void *>(model.meshes[0]));
          xmodel::XModelMeshMaterials materials{};
          if (peek(model.meshMaterials, materials)) {
            for (int k = 0; k < materials.numMaterials && k < 4; ++k) {
              const void *material = nullptr;
              if (!peek(materials.materials + k, material)) {
                break;
              }
              made_of += std::format(" {:p}", material);
            }
          }
          const std::string display =
              plain(text_at(field<const char *>(body.bytes.data(),
                                                BODY_DISPLAY_NAME)));
          if (std::ranges::any_of(found, [&](const look &one) {
                return one.source == source && one.name == display &&
                       one.made_of == made_of;
              })) {
            continue;
          }

          look out;
          out.source = source;
          out.in_place = !own_table;
          out.made_of = std::move(made_of);
          out.body = static_cast<uint32_t>(at_body);
          out.style = static_cast<uint32_t>(at_style);
          out.added = added;
          const uint8_t *bytes = body.bytes.data();
          const std::string body_name =
              text_at(field<const char *>(bytes, BODY_NAME));
          out.name = display;
          // the portrait is the table's own character: not the one a mod put
          // in its place
          if (own_table) {
            out.icon = plain(text_at(field<const char *>(bytes, BODY_ICON)));
          }
          const char *mesh_name = nullptr;
          if (peek(model.meshes[0], mesh_name)) {
            out.hint = clean(text_at(mesh_name));
          }
          out.id = look_id(source + "|" + body_name + "|" +
                           text_at(field<const char *>(item.data(), ITEM_NAME)) +
                           "|" + std::to_string(j));
          found.push_back(std::move(out));
        }
      }
    }
  }

  // the table in use: the lists with outfits added go in, the others out
  for (slots_t &slot : slots) {
    if (!slot.usable) {
      continue;
    }
    record &rec = *slot.rec;
    const auto count = static_cast<uint32_t>(slot.items.size());
    const uint8_t *now_items = nullptr;
    uint32_t now_count = 0;
    if (!peek(slot.body + BODY_STYLES, now_items) ||
        !peek(slot.body + BODY_NUM_STYLES, now_count)) {
      continue;
    }
    if (count > slot.own) {
      const uint8_t *target = rec.buffer.get();
      if (now_items == target && now_count > count) {
        poke(slot.body + BODY_NUM_STYLES, count);
        now_count = count;
      }
      for (uint32_t s = 0; s < count; ++s) {
        uint8_t *to = rec.buffer.get() + s * ITEM_SIZE;
        if (std::memcmp(to, slot.items[s].data(), ITEM_SIZE) != 0) {
          std::memcpy(to, slot.items[s].data(), ITEM_SIZE);
        }
      }
      if (now_items != target && !poke(slot.body + BODY_STYLES, target)) {
        continue;
      }
      if (now_count != count) {
        poke(slot.body + BODY_NUM_STYLES, count);
      }
    } else if (now_items == rec.buffer.get()) {
      poke(slot.body + BODY_NUM_STYLES, rec.original_count);
      poke(slot.body + BODY_STYLES, rec.original_items);
    }
  }

  build_ms = std::chrono::duration<double, std::milli>(
                 std::chrono::steady_clock::now() - started)
                 .count();
  int added = 0;
  std::set<std::string> sources;
  for (const look &one : found) {
    added += one.added ? 1 : 0;
    sources.insert(one.source);
  }
  const bool changed = found.size() != looks.size() || dropped != dropped_looks;
  looks = std::move(found);
  dropped_looks = dropped;
  if (changed) {
    xerces_runtime::log_info(
        "xerces.char.looks",
        std::format("Character slots: {} characters from {} sources beside the "
                    "table in use ({} outfits added, {} left out for room), "
                    "{:.1f} ms",
                    looks.size(), sources.size(), added, dropped, build_ms),
        std::format("{{\"looks\":{},\"sources\":{},\"added\":{},\"dropped\":{},"
                    "\"table_owner\":\"{}\"}}",
                    looks.size(), sources.size(), added, dropped, live_owner));
  }
  return true;
}

// The saved pick names a look by its id: its slot depends on the mods loaded
void fix_saved() {
  const int id = game::get_dvar_int("xerces_char_saved_look").value_or(0);
  if (id <= 0) {
    return;
  }
  int slot = 0;
  for (const look &one : looks) {
    if (one.id == static_cast<uint32_t>(id)) {
      slot = static_cast<int>(one.body * 16 + one.style + 1);
      break;
    }
  }
  if (game::get_dvar_int("xerces_char_saved").value_or(0) != slot) {
    game::set_dvar_int("xerces_char_saved", slot, false);
  }
  if (slot == 0) {
    game::set_dvar_int("xerces_char_saved_look", 0, false);
  }
}

// The looks are worked out again whenever the game has linked an asset since
// (a level or a mod loaded) or uses another table
void ensure() {
  if (!game::com::Com_SessionMode_IsMode(game::eModes::ZOMBIES) ||
      !game::sys::Sys_IsDatabaseReady()) {
    return;
  }
  const void *in_use =
      game::bg::BG_GetCharacterBodyType(game::eModes::ZOMBIES, 0);
  if (!in_use) {
    return;
  }
  std::scoped_lock lock(looks_mutex);
  const uint64_t links = mod_stack::link_generation();
  if (links == built_links && in_use == built_table) {
    return;
  }
  built_links = links;
  built_table = in_use;
  if (build(static_cast<const uint8_t *>(in_use))) {
    fix_saved();
  } else {
    looks.clear();
  }
}

bool was_in_level = false;

void frame() {
  const bool in_level = game::com::Com_IsInGame() &&
                        !game::com::Com_IsRunningUILevel() &&
                        game::com::Com_SessionMode_IsMode(game::eModes::ZOMBIES);
  if (in_level) {
    ensure();
  } else if (was_in_level) {
    std::scoped_lock lock(looks_mutex);
    restore_all();
    looks.clear();
    built_table = nullptr;
    built_links = UINT64_MAX;
  }
  was_in_level = in_level;
}

std::string title_of(const std::string &source) {
  if (source.empty()) {
    return "the game";
  }
  const std::string title = mod_stack::workshop_title(source);
  return title.empty() ? source : title;
}

// xerces_char_looks: the looks as they are now, to the log and the console
void list_looks() {
  ensure();
  std::scoped_lock lock(looks_mutex);
  std::string text =
      std::format("{} characters beside the table in use, {} left out, {:.1f} ms",
                  looks.size(), dropped_looks, build_ms);
  std::string source = "\x01";
  for (const look &one : looks) {
    if (one.source != source) {
      source = one.source;
      text += "\n" + title_of(source) + ":";
    }
    text += std::format(" [{} {}:{}{} {}]", one.name, one.body, one.style,
                        one.added ? "+" : "", one.hint);
  }
  xerces_runtime::log_info("xerces.char.looks.list", text);
  printf("%s\n", text.c_str());
}

// One call from a script: traced, skipped when the player is locked, else the
// game's own
void locked_call(const char *name, BuiltinMethod original, scriptInstance_t inst,
                 scr_entref_t *entref) {
  if (tracing) {
    note(name, inst, entref, false);
  }
  if (locked(entref)) {
    if (tracing) {
      note((std::string(name) + " (skipped: locked)").c_str(), inst, entref,
           false);
    }
    return;
  }
  // an outfit Xerces added must be there before the game is asked for it
  ensure();
  original(inst, entref);
}

void SetCharacterBodyType(scriptInstance_t inst, scr_entref_t *entref) {
  locked_call("SetCharacterBodyType", set_body_type_orig, inst, entref);
}
void SetCharacterBodyStyle(scriptInstance_t inst, scr_entref_t *entref) {
  locked_call("SetCharacterBodyStyle", set_body_style_orig, inst, entref);
}
void SetCharacterHelmetStyle(scriptInstance_t inst, scr_entref_t *entref) {
  locked_call("SetCharacterHelmetStyle", set_helmet_style_orig, inst, entref);
}
} // namespace

std::string looks_text() {
  if (!game::com::Com_IsInGame() || game::com::Com_IsRunningUILevel()) {
    return {};
  }
  ensure();
  std::scoped_lock lock(looks_mutex);
  std::string out;
  for (const look &one : looks) {
    out += std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", one.source,
                       one.name, one.hint, one.icon, one.body, one.style, one.id,
                       one.added ? 1 : 0, one.in_place ? 1 : 0);
  }
  return out;
}

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_character_slots");

public:
  void post_unpack() override {
    if (!game::is_client() || !game::is_new_client()) {
      return;
    }
    const game::DvarFlags no_flags{};
    game::register_dvar_int(
        "xerces_char_lock", 0, 0, INT32_MAX, no_flags,
        "Xerces' Zombies character slots: the players (bit per client) whose "
        "character other scripts may not change.");
    game::register_dvar_int(
        "xerces_char_apply", 0, 0, 1, no_flags,
        "Xerces' Zombies character slots: 1 while Xerces' own script sets a "
        "player's character.");
    const game::DvarFlags saved{.archive = 1};
    game::register_dvar_int(
        "xerces_char_saved", 0, 0, 4096, saved,
        "Xerces' Zombies character slots: the character you last picked "
        "(index * 16 + outfit + 1, 0 for none). Put on again when you host a "
        "match.");
    game::register_dvar_int(
        "xerces_char_saved_look", 0, 0, INT32_MAX, saved,
        "Xerces' Zombies character slots: which mod's character the saved "
        "pick is (0: one of the table in use), so it is found again when the "
        "mods loaded change.");
    using namespace game::scr::builtin::table;
    set_body_type_orig = player_methods->SetCharacterBodyType.actionFunc;
    player_methods->SetCharacterBodyType.actionFunc = SetCharacterBodyType;
    set_body_style_orig = player_methods->SetCharacterBodyStyle.actionFunc;
    player_methods->SetCharacterBodyStyle.actionFunc = SetCharacterBodyStyle;
    set_helmet_style_orig = player_methods->SetCharacterHelmetStyle.actionFunc;
    player_methods->SetCharacterHelmetStyle.actionFunc = SetCharacterHelmetStyle;

    scheduler::loop(frame, scheduler::main);
    command::add("xerces_char_looks", [] { list_looks(); });

    wchar_t buffer[8];
    if (GetEnvironmentVariableW(L"XERCES_CHAR_TRACE", buffer, 8) == 0 ||
        buffer[0] != L'1') {
      return;
    }
    tracing = true;
    set_model_orig =
        game::scr::builtin::table::gscr::builtin_methods->SetModel.actionFunc;
    game::scr::builtin::table::gscr::builtin_methods->SetModel.actionFunc =
        SetModel;
    attach_orig = common_methods->Attach.actionFunc;
    common_methods->Attach.actionFunc = Attach;
    detach_all_orig = common_methods->DetachAll.actionFunc;
    common_methods->DetachAll.actionFunc = DetachAll;
    set_player_character_orig = player_methods->SetPlayerCharacter.actionFunc;
    player_methods->SetPlayerCharacter.actionFunc = SetPlayerCharacter;
  }
};
} // namespace xerces_character_slots

REGISTER_COMPONENT(xerces_character_slots::component)
