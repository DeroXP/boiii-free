#include "mods_info.hpp"

#include <algorithm>

namespace xerces_core {
const installed_content *chosen_content(const catalog_state &catalog,
                                        const plan &resolved,
                                        std::string_view id) {
  const auto at_path =
      [&](const std::filesystem::path &path) -> const installed_content * {
    for (const auto &item : catalog.items) {
      if (item.path == path) {
        return &item;
      }
    }
    return nullptr;
  };
  for (const auto &entry : resolved.stack) {
    if (entry.id == id) {
      if (const auto *item = at_path(entry.path)) {
        return item;
      }
    }
  }
  if (resolved.map && resolved.map->id == id) {
    if (const auto *item = at_path(resolved.map->path)) {
      return item;
    }
  }
  return catalog.find(id);
}

std::optional<mod_details> describe_mod(const catalog_state &catalog,
                                        const mode_profile *profile,
                                        const plan &resolved,
                                        const std::string &mode,
                                        const std::string &id) {
  const installed_content *item = chosen_content(catalog, resolved, id);
  std::size_t position = 0; // 1-based in the load order
  if (profile) {
    if (const auto it = std::ranges::find(profile->mods, id);
        it != profile->mods.end()) {
      position = std::size_t(it - profile->mods.begin()) + 1;
    }
  }
  const bool on = position != 0;
  const bool picked = profile && profile->map && *profile->map == id;
  if (!item && !on && !picked) {
    return std::nullopt;
  }
  mod_details out;
  out.id = id;
  out.installed = item != nullptr;
  if (profile) {
    out.enabled = profile->mods.size();
    out.position = position;
    out.map_picked = picked;
    if (const auto pin = profile->pins.find(id); pin != profile->pins.end()) {
      out.pinned = pin->second;
    }
  }
  for (std::size_t k = 0; k < resolved.stack.size(); ++k) {
    if (resolved.stack[k].id == id) {
      out.loads_at = k + 1;
      out.dependency = !resolved.stack[k].requested;
    }
  }
  if (!item) {
    out.name = id;
    if (id.starts_with("ws.")) {
      out.workshop_id = id.substr(3);
    }
  } else {
    const manifest &info = item->info;
    out.is_map = info.kind == content_kind::map;
    out.name = info.name.empty() ? id : mods_plain_name(info.name);
    out.legacy = info.legacy;
    out.version =
        info.legacy
            ? std::string("legacy") +
                  (info.ver.build.empty() ? "" : " (" + info.ver.build + ")")
            : info.ver.to_string();
    const std::size_t index = std::size_t(item - catalog.items.data());
    if (index < catalog.sources.size()) {
      out.source = catalog.sources[index];
    }
    out.folder = item->path.generic_string();
    out.folder_name = item->folder;
    if (info.workshop_id) {
      out.workshop_id = std::to_string(*info.workshop_id);
    }
    out.files = item->files.size();
    out.zones = count_zone_files(item->files);
    out.content_hash = item->content_hash;
    out.modes = info.modes;
    out.supports_mode = info.supports_mode(mode);
    out.authors = info.authors;
    out.license = info.license;
    if (!info.legacy) {
      out.provides = info.provides.size();
      out.overrides = info.overrides.size();
      out.extends = info.extends.size();
      out.hooks = info.hooks.size();
      for (const auto &dep : info.dependencies) {
        out.needs.push_back(dep.id + " " + dep.range_text +
                            (dep.optional ? " (optional)" : ""));
      }
      for (const auto &c : info.conflicts) {
        out.conflicts.push_back(c.id);
      }
    }
    out.capabilities = mods_capabilities(info.caps);
    out.settings = info.settings;
  }

  // Problems, as the screen pins them: the plan's notes that name the mod
  // (by id or by its name), its folder's scan notes, a broken manifest.
  std::vector<std::pair<std::string, std::string>> names;
  for (const auto &content : catalog.items) {
    names.emplace_back(content.info.id, content.info.name.empty()
                                            ? content.info.id
                                            : content.info.name);
  }
  if (profile) {
    for (const auto &enabled : profile->mods) {
      names.emplace_back(enabled, enabled);
    }
  }
  const mods_notes notes = mods_attribute(resolved, names);
  if (const auto it = notes.by_id.find(id); it != notes.by_id.end()) {
    for (const auto &note : it->second) {
      if (note.level != severity::info) {
        out.problems.push_back(note);
      }
    }
  }
  if (item) {
    for (const auto &note : mods_folder_notes(catalog.diags, item->folder)) {
      if (note.level != severity::info) {
        out.problems.push_back(note);
      }
    }
    if (!item->valid_manifest) {
      out.problems.push_back({severity::error, "mod.manifest",
                              "Its xerces.mod.json has errors; the plan will "
                              "not load it."});
    }
  }
  const bool is_plan_map = resolved.map && resolved.map->id == id;
  if (on || is_plan_map || out.loads_at != 0) {
    for (const auto &fix : resolved.fixes) {
      if (fix.id == id) {
        out.fixes.push_back(fix);
      }
    }
    // A missing, pinned or outdated dependency is filed under the
    // dependency's id; the mod that needs it offers the fix.
    if (item) {
      for (const auto &dep : item->info.dependencies) {
        for (const auto &fix : resolved.fixes) {
          if (fix.id == dep.id && fix.id != id &&
              (fix.kind == "install" || fix.kind == "update" ||
               fix.kind == "unpin")) {
            out.fixes.push_back(fix);
          }
        }
      }
    }
  }
  for (const auto &why : resolved.explanations) {
    if (why.subject == id || why.object == id) {
      out.why.push_back(why.message);
    }
  }
  return out;
}
} // namespace xerces_core
