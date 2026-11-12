#pragma once

#include <string>

// Zombies character slots (docs/DESIGN.md 7.10 and 7.12).
namespace xerces_character_slots {
// Every character the loaded mods and the game have beside the table in use,
// one line each: source (a Workshop id, "" the game's own), name, model hint,
// portrait image, character index, outfit index, id (for the saved pick), 1
// when the outfit is one Xerces added, 1 when the name is the character whose
// models the mod replaces (the hint then says who it is); tab separated. For the CHARACTERS
// screen (data/ui_scripts/xerces_characters). "" outside a Zombies match.
std::string looks_text();
} // namespace xerces_character_slots
