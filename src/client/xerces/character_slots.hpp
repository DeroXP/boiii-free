#pragma once

#include <string>

// Zombies character slots (docs/DESIGN.md 7.10 and 7.12).
namespace xerces_character_slots {
// Every character there is to pick: the table in use's own and every other
// mod's and the game's. The first line is "@", then the Workshop id of the mod
// whose table is in use ("" the game's or a custom map's). Then one line
// each: source (a Workshop id, "" the game's own), name, model hint,
// portrait image, character index, outfit index, id (for the saved pick), 1
// when the outfit is one Xerces added, 1 when the name is the character whose
// models the mod replaces (the hint then says who it is); tab separated. For the CHARACTERS
// screen (data/ui_scripts/xerces_characters). "" outside a Zombies match.
std::string looks_text();
} // namespace xerces_character_slots
