#pragma once

// A friend's mods, for joining their lobby (docs/DESIGN.md 10.12).
//
// With a modded invite the host's launcher shares its stack as a pack on the
// backend and puts the pack's code in the invite. The joiner's launcher
// fetches the pack and asks check_invite_pack which of its mods are here and
// which are not. Missing ones are fetched the way a player gets any mod: its
// Workshop page opens in Steam, the player subscribes, and Steam downloads
// it (nothing here goes around Steam). Once every mod is here the game
// starts with the pack's mods, in the pack's order, and joins.
//
// Headless and pure.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "launcher_state.hpp"
#include "pack.hpp"

namespace xerces_core {
struct invite_pack_check {
  // The ids to launch with (launch_inputs::mods), highest priority first:
  // the pack's order, each as it is installed here (an item matched by its
  // Workshop id may carry another id here).
  std::vector<std::string> launch_mods;
  struct missing_mod {
    std::string id;
    std::optional<std::uint64_t> workshop_id; // nullopt: not on the Workshop
  };
  std::vector<missing_mod> missing; // the pack's order
  bool complete() const { return missing.empty(); }
  // Missing mods that are not on the Workshop: the player has to get these
  // from the host some other way.
  std::size_t not_on_workshop() const;
};

// Matches by id first (the highest installed version), then by Workshop id
// (a Workshop folder named for it, or a manifest that names it).
invite_pack_check check_invite_pack(const pack &value,
                                    const catalog_state &catalog);

// A pack's Workshop pages to open: steam://url/CommunityFilePage/<id>.
std::string workshop_page_url(std::uint64_t workshop_id);
} // namespace xerces_core
