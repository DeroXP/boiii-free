#include "invite_pack.hpp"

#include <algorithm>

namespace xerces_core {
std::size_t invite_pack_check::not_on_workshop() const {
  return static_cast<std::size_t>(std::ranges::count_if(
      missing, [](const missing_mod &mod) { return !mod.workshop_id; }));
}

namespace {
const installed_content *by_workshop_id(const catalog_state &catalog,
                                        const std::uint64_t workshop_id) {
  const std::string folder = std::to_string(workshop_id);
  for (std::size_t i = 0; i < catalog.items.size(); ++i) {
    const installed_content &item = catalog.items[i];
    const bool workshop =
        i < catalog.sources.size() && catalog.sources[i] == "workshop";
    if ((workshop && item.folder == folder) ||
        item.info.workshop_id == workshop_id) {
      return &item;
    }
  }
  return nullptr;
}
} // namespace

invite_pack_check check_invite_pack(const pack &value,
                                    const catalog_state &catalog) {
  invite_pack_check out;
  for (const std::string &id : value.order) {
    const auto entry = std::ranges::find(value.mods, id, &pack_mod::id);
    const std::optional<std::uint64_t> workshop_id =
        entry != value.mods.end() ? entry->workshop_id : std::nullopt;
    const installed_content *found = catalog.find(id);
    if (!found && workshop_id) {
      found = by_workshop_id(catalog, *workshop_id);
    }
    if (found) {
      if (std::ranges::find(out.launch_mods, found->info.id) ==
          out.launch_mods.end()) {
        out.launch_mods.push_back(found->info.id);
      }
    } else {
      out.missing.push_back({id, workshop_id});
    }
  }
  return out;
}

std::string workshop_page_url(const std::uint64_t workshop_id) {
  return "steam://url/CommunityFilePage/" + std::to_string(workshop_id);
}
} // namespace xerces_core
