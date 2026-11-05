// Mods (docs/DESIGN.md 7.3): the heart of multi-mod. The mode switch (shared
// with Home), search and filters, the load order with enable switches,
// moves (buttons, Ctrl+Up/Down, mouse drag) and Ctrl+Z, the planner's
// conflict notices next to the mods they name, the maps, the rescan and a
// details pane for the selected mod. The rules that do not need the UI (hand
// edits, undo, search, which mod a diagnostic names) live in
// xerces_core/mods_edit.hpp and are unit tested.

#include <RmlUi/Core.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <xerces_core/mods_edit.hpp>

#include "../app.hpp"
#include "../log.hpp"

namespace xerces_launcher {
namespace {
namespace xc = xerces_core;

struct note_row {
  Rml::String text;
  bool error = false;
};
struct order_row {
  Rml::String key, id, n, name, ver, state, loads;
  bool problem = false, warn = false, missing = false, main = false;
  bool first = false, last = false;
  Rml::Vector<note_row> notes;
};
struct dep_row {
  Rml::String key, name, ver;
};
struct mod_row {
  Rml::String key, id, name, ver, source, ws, state;
  bool on = false, problem = false, warn = false, legacy = false;
  Rml::Vector<note_row> notes;
};
struct map_row {
  Rml::String key, id, name, source;
  bool on = false;
};
struct field_row {
  Rml::String l, v;
};
struct fix_row {
  Rml::String label;
  int index = 0;
};

std::string two_digits(std::size_t value) {
  char text[16];
  std::snprintf(text, sizeof(text), "%02zu", value);
  return text;
}

const char *mode_name(const std::string &mode) {
  return mode == "mp" ? "Multiplayer" : mode == "cp" ? "Campaign" : "Zombies";
}

const char *source_name(const std::string &source) {
  return source == "workshop"   ? "Workshop"
         : source == "mods"     ? "mods"
         : source == "usermaps" ? "usermaps"
                                : "";
}

std::string join(const std::vector<std::string> &items) {
  std::string out;
  for (const auto &item : items) {
    out += (out.empty() ? "" : ", ") + item;
  }
  return out;
}

// What the details pane shows: one catalog item (a version on disk), or an
// enabled id that is not installed.
struct selection {
  char section = 0; // 'o' load order, 'c' catalog item, 'n' no map
  std::string id;
  fs::path path; // the catalog item's folder, to find it again after a scan
};

class mods_view final : public view {
public:
  const char *id() const override { return "mods"; }

  bool bind(Rml::Context *context, app &owner) override {
    owner_ = &owner;
    Rml::DataModelConstructor model = context->CreateDataModel("mods");
    if (!model) {
      return false;
    }
    if (auto s = model.RegisterStruct<note_row>()) {
      s.RegisterMember("text", &note_row::text);
      s.RegisterMember("error", &note_row::error);
    }
    model.RegisterArray<Rml::Vector<note_row>>();
    if (auto s = model.RegisterStruct<order_row>()) {
      s.RegisterMember("key", &order_row::key);
      s.RegisterMember("id", &order_row::id);
      s.RegisterMember("n", &order_row::n);
      s.RegisterMember("name", &order_row::name);
      s.RegisterMember("ver", &order_row::ver);
      s.RegisterMember("state", &order_row::state);
      s.RegisterMember("loads", &order_row::loads);
      s.RegisterMember("problem", &order_row::problem);
      s.RegisterMember("warn", &order_row::warn);
      s.RegisterMember("missing", &order_row::missing);
      s.RegisterMember("main", &order_row::main);
      s.RegisterMember("first", &order_row::first);
      s.RegisterMember("last", &order_row::last);
      s.RegisterMember("notes", &order_row::notes);
    }
    if (auto s = model.RegisterStruct<dep_row>()) {
      s.RegisterMember("key", &dep_row::key);
      s.RegisterMember("name", &dep_row::name);
      s.RegisterMember("ver", &dep_row::ver);
    }
    if (auto s = model.RegisterStruct<mod_row>()) {
      s.RegisterMember("key", &mod_row::key);
      s.RegisterMember("id", &mod_row::id);
      s.RegisterMember("name", &mod_row::name);
      s.RegisterMember("ver", &mod_row::ver);
      s.RegisterMember("source", &mod_row::source);
      s.RegisterMember("ws", &mod_row::ws);
      s.RegisterMember("state", &mod_row::state);
      s.RegisterMember("on", &mod_row::on);
      s.RegisterMember("problem", &mod_row::problem);
      s.RegisterMember("warn", &mod_row::warn);
      s.RegisterMember("legacy", &mod_row::legacy);
      s.RegisterMember("notes", &mod_row::notes);
    }
    if (auto s = model.RegisterStruct<map_row>()) {
      s.RegisterMember("key", &map_row::key);
      s.RegisterMember("id", &map_row::id);
      s.RegisterMember("name", &map_row::name);
      s.RegisterMember("source", &map_row::source);
      s.RegisterMember("on", &map_row::on);
    }
    if (auto s = model.RegisterStruct<field_row>()) {
      s.RegisterMember("l", &field_row::l);
      s.RegisterMember("v", &field_row::v);
    }
    if (auto s = model.RegisterStruct<fix_row>()) {
      s.RegisterMember("label", &fix_row::label);
      s.RegisterMember("index", &fix_row::index);
    }
    model.RegisterArray<Rml::Vector<order_row>>();
    model.RegisterArray<Rml::Vector<dep_row>>();
    model.RegisterArray<Rml::Vector<mod_row>>();
    model.RegisterArray<Rml::Vector<map_row>>();
    model.RegisterArray<Rml::Vector<field_row>>();
    model.RegisterArray<Rml::Vector<fix_row>>();
    model.RegisterArray<Rml::Vector<Rml::String>>();

    model.Bind("mode", &mode_);
    model.Bind("query", &query_);
    model.Bind("search_hint", &search_hint_);
    model.Bind("filter", &filter_);
    model.Bind("n_all", &n_all_);
    model.Bind("n_enabled", &n_enabled_);
    model.Bind("n_problems", &n_problems_);
    model.Bind("n_legacy", &n_legacy_);
    model.Bind("scanning", &scanning_);
    model.Bind("scan_text", &scan_text_);
    model.Bind("scan_detail", &scan_detail_);
    model.Bind("empty_title", &empty_title_);
    model.Bind("empty_text", &empty_text_);
    model.Bind("empty_game", &empty_game_);
    model.Bind("order", &order_);
    model.Bind("order_count", &order_count_);
    model.Bind("order_empty", &order_empty_);
    model.Bind("pack_note", &pack_note_);
    model.Bind("plan_line", &plan_line_);
    model.Bind("general", &general_);
    model.Bind("deps", &deps_);
    model.Bind("mods", &mods_);
    model.Bind("mods_count", &mods_count_);
    model.Bind("list_empty", &list_empty_);
    model.Bind("list_filtered", &list_filtered_);
    model.Bind("maps", &maps_);
    model.Bind("maps_count", &maps_count_);
    model.Bind("map_none", &map_none_);
    model.Bind("has_maps", &has_maps_);
    model.Bind("sel", &sel_key_);
    model.Bind("drag_from", &drag_from_);
    model.Bind("drop_at", &drop_at_);
    model.Bind("can_undo", &can_undo_);

    model.Bind("d_has", &d_has_);
    model.Bind("d_kind", &d_kind_);
    model.Bind("d_name", &d_name_);
    model.Bind("d_id", &d_id_);
    model.Bind("d_ws", &d_ws_);
    model.Bind("d_path", &d_path_);
    model.Bind("d_legacy", &d_legacy_);
    model.Bind("d_toggle", &d_toggle_);
    model.Bind("d_can_toggle", &d_can_toggle_);
    model.Bind("d_fields", &d_fields_);
    model.Bind("d_notes", &d_notes_);
    model.Bind("d_fixes", &d_fixes_);
    model.Bind("d_fix_text", &d_fix_text_);
    model.Bind("d_why", &d_why_);
    model.Bind("d_caps", &d_caps_);
    model.Bind("d_caps_empty", &d_caps_empty_);
    model.Bind("d_settings", &d_settings_);
    model.Bind("d_settings_empty", &d_settings_empty_);

    const auto arg_string = [](const Rml::VariantList &args, std::size_t i) {
      return i < args.size() ? args[i].Get<Rml::String>() : Rml::String();
    };
    const auto arg_int = [](const Rml::VariantList &args, std::size_t i) {
      return i < args.size() ? args[i].Get<int>() : 0;
    };
    model.BindEventCallback(
        "pick", [this, arg_string](Rml::DataModelHandle, Rml::Event &,
                                   const Rml::VariantList &args) {
          owner_->set_mode(arg_string(args, 0));
        });
    model.BindEventCallback(
        "set_filter", [this, arg_string](Rml::DataModelHandle, Rml::Event &,
                                         const Rml::VariantList &args) {
          sync();
          filter_ = arg_string(args, 0);
          apply_filter();
        });
    model.BindEventCallback(
        "select", [this, arg_string](Rml::DataModelHandle, Rml::Event &,
                                     const Rml::VariantList &args) {
          sync();
          select(arg_string(args, 0));
        });
    model.BindEventCallback(
        "toggle", [this, arg_string](Rml::DataModelHandle, Rml::Event &event,
                                     const Rml::VariantList &args) {
          sync();
          toggle(arg_string(args, 0), event.GetCurrentElement());
        });
    model.BindEventCallback(
        "move", [this, arg_int](Rml::DataModelHandle, Rml::Event &event,
                                const Rml::VariantList &args) {
          sync();
          const Rml::Element *button = event.GetCurrentElement();
          const std::string prefix =
              button ? button->GetId().substr(0, 3) : std::string();
          move_index(arg_int(args, 0), arg_int(args, 1), prefix);
        });
    model.BindEventCallback(
        "pick_map", [this, arg_string](Rml::DataModelHandle, Rml::Event &,
                                       const Rml::VariantList &args) {
          sync();
          pick_map(arg_string(args, 0));
        });
    model.BindEventCallback("fix",
                            [this, arg_int](Rml::DataModelHandle, Rml::Event &,
                                            const Rml::VariantList &args) {
                              sync();
                              apply_fix(arg_int(args, 0));
                            });
    model.BindEventCallback(
        "act", [this, arg_string](Rml::DataModelHandle, Rml::Event &,
                                  const Rml::VariantList &args) {
          sync();
          act(arg_string(args, 0));
        });
    // A click on a row's notes or empty space would focus that div (every
    // element can take focus in RmlUi); the row's main button takes it.
    model.BindEventCallback(
        "row_down", [this, arg_string](Rml::DataModelHandle, Rml::Event &event,
                                       const Rml::VariantList &args) {
          for (const Rml::Element *e = event.GetTargetElement();
               e && e != event.GetCurrentElement(); e = e->GetParentNode()) {
            if (e->GetTagName() == "button") {
              return; // the button takes focus itself
            }
          }
          focus_now(arg_string(args, 0));
        });
    // Mouse drag in the load order (RmlUi's drag: drag-drop on the rows).
    model.BindEventCallback("drag_start",
                            [this, arg_int](Rml::DataModelHandle, Rml::Event &,
                                            const Rml::VariantList &args) {
                              sync();
                              drag_from_ = arg_int(args, 0);
                              drop_at_ = -1;
                              dirty({"drag_from", "drop_at"});
                            });
    // dragover and dragout bubble, and RmlUi sends them to every element
    // that enters or leaves the hover chain: moving from a row's name to its
    // padding sends a dragout from the name that would reach the row while
    // the cursor is still over it. Only the row's own enter and leave count.
    const auto own = [](const Rml::Event &event) {
      return event.GetTargetElement() == event.GetCurrentElement();
    };
    model.BindEventCallback(
        "drag_over",
        [this, arg_int, own](Rml::DataModelHandle, Rml::Event &event,
                             const Rml::VariantList &args) {
          if (own(event) && drag_from_ >= 0 && drop_at_ != arg_int(args, 0)) {
            drop_at_ = arg_int(args, 0);
            dirty({"drop_at"});
          }
        });
    model.BindEventCallback("drag_out",
                            [this, arg_int, own](Rml::DataModelHandle,
                                                 Rml::Event &event,
                                                 const Rml::VariantList &args) {
                              if (own(event) && drop_at_ == arg_int(args, 0)) {
                                drop_at_ = -1;
                                dirty({"drop_at"});
                              }
                            });
    model.BindEventCallback("drag_drop",
                            [this, arg_int](Rml::DataModelHandle, Rml::Event &,
                                            const Rml::VariantList &args) {
                              sync();
                              const int from = drag_from_;
                              const int to = arg_int(args, 0);
                              drag_from_ = drop_at_ = -1;
                              dirty({"drag_from", "drop_at"});
                              if (from >= 0) {
                                move_index(from, to - from, "om-");
                              }
                            });
    model.BindEventCallback(
        "drag_end",
        [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) {
          if (drag_from_ >= 0 || drop_at_ >= 0) {
            drag_from_ = drop_at_ = -1;
            dirty({"drag_from", "drop_at"});
          }
        });
    handle_ = model.GetModelHandle();
    return true;
  }

  void shown() override { sync(); }

  void hidden() override {
    drag_from_ = drop_at_ = -1;
    dirty({"drag_from", "drop_at"});
  }

  void update(double) override {
    if (!pending_focus_.empty()) {
      // The model changed this frame; its elements exist after the next
      // context update, so focus waits one frame.
      if (pending_wait_ > 0) {
        --pending_wait_;
      } else {
        apply_pending_focus();
      }
    }
    sync();
    if (query_ != applied_query_) {
      apply_filter();
    }
    first_focus();
    keep_focus_visible();
  }

  bool key(Rml::Input::KeyIdentifier key, int modifiers) override {
    // The Win32 backend also reports Caps Lock, Num Lock and Scroll Lock
    // while they are toggled on; only keys held down count as a chord, or
    // every plain key below would stop working with Num Lock on.
    const int mods = modifiers & (Rml::Input::KM_CTRL | Rml::Input::KM_SHIFT |
                                  Rml::Input::KM_ALT | Rml::Input::KM_META);
    const Rml::Element *focus =
        owner_->context() ? owner_->context()->GetFocusElement() : nullptr;
    if (focus && focus->GetId() == "search" && mods == 0 &&
        key == Rml::Input::KI_RETURN) {
      sync();
      focus_now(!mods_.empty() ? "lm-0" : !order_.empty() ? "om-0" : "");
      return true;
    }
    if (mods == 0 && focus &&
        (key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN ||
         key == Rml::Input::KI_LEFT || key == Rml::Input::KI_RIGHT)) {
      if (navigate(const_cast<Rml::Element *>(focus), key)) {
        return true;
      }
    }
    if (focus &&
        (focus->GetTagName() == "input" || focus->GetTagName() == "textarea" ||
         focus->GetTagName() == "select")) {
      return false; // typing in the search box
    }
    const bool ctrl = (mods & Rml::Input::KM_CTRL) != 0;
    const bool others = (mods & (Rml::Input::KM_SHIFT | Rml::Input::KM_ALT |
                                 Rml::Input::KM_META)) != 0;
    if (ctrl && !others &&
        (key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN)) {
      sync();
      move_selected(key == Rml::Input::KI_UP ? -1 : +1);
      return true;
    }
    if (ctrl && !others && key == Rml::Input::KI_Z) {
      sync();
      undo();
      return true;
    }
    if (mods == 0 && key == Rml::Input::KI_DELETE) {
      sync();
      if (sel_.section == 'o' || sel_.section == 'c') {
        if (enabled(sel_.id)) {
          toggle(sel_.id, focus);
          return true;
        }
      }
      return false;
    }
    if (mods == 0 && key == Rml::Input::KI_F5) {
      act("rescan");
      return true;
    }
    return false;
  }

private:
  // ------------------------------------------------------------ helpers
  services &svc() { return owner_->svc(); }

  void dirty(std::initializer_list<const char *> names) {
    for (const char *name : names) {
      handle_.DirtyVariable(name);
    }
  }

  // The footer line. It becomes RML inside this screen's data model, and
  // app::escape leaves braces alone, so a Workshop title or a pack name with
  // "{{" or "}}" in it would be read as a data expression (showing a model
  // value, or nothing). A space between doubled braces keeps it plain text.
  void notify(const std::string &text, bool error = false) {
    std::string out;
    out.reserve(text.size() + 4);
    for (const char c : text) {
      if ((c == '{' || c == '}') && !out.empty() && out.back() == c) {
        out.push_back(' ');
      }
      out.push_back(c);
    }
    owner_->notify(out, error);
  }

  const xc::mode_profile *profile() const {
    const auto &profiles = owner_->svc().config().profiles;
    const auto it = profiles.find(mode_);
    return it == profiles.end() ? nullptr : &it->second;
  }

  bool enabled(const std::string &id) const {
    const xc::mode_profile *p = profile();
    return p && std::ranges::find(p->mods, id) != p->mods.end();
  }

  std::size_t position(const std::string &id) const {
    const xc::mode_profile *p = profile();
    if (!p) {
      return 0;
    }
    const auto it = std::ranges::find(p->mods, id);
    return it == p->mods.end() ? 0 : std::size_t(it - p->mods.begin()) + 1;
  }

  // The version the plan uses for an id, else the first one installed.
  const xc::installed_content *chosen(const std::string &id) const {
    const auto &catalog = owner_->svc().catalog();
    for (const auto &entry : plan_.stack) {
      if (entry.id != id) {
        continue;
      }
      for (const auto &item : catalog.items) {
        if (item.path == entry.path) {
          return &item;
        }
      }
    }
    if (plan_.map && plan_.map->id == id) {
      for (const auto &item : catalog.items) {
        if (item.path == plan_.map->path) {
          return &item;
        }
      }
    }
    return catalog.find(id);
  }

  std::size_t index_of(const xc::installed_content *item) const {
    return std::size_t(item - owner_->svc().catalog().items.data());
  }

  // Translated here: the map rows join it with more text, and a bound
  // string is only translated by RmlUi when it is one whole key.
  std::string source_of(const xc::installed_content *item) const {
    const auto &sources = owner_->svc().catalog().sources;
    const std::size_t i = index_of(item);
    const char *name = i < sources.size() ? source_name(sources[i]) : "";
    return *name ? owner_->tr(name) : std::string();
  }

  std::string version_text(const xc::manifest &info) const {
    return info.legacy ? owner_->tr("legacy") : info.ver.to_string();
  }

  std::string workshop_of(const xc::installed_content &item) const {
    if (item.info.workshop_id) {
      return std::to_string(*item.info.workshop_id);
    }
    return "";
  }

  static std::string plain(const xc::manifest &info, const std::string &id) {
    return info.name.empty() ? id : xc::mods_plain_name(info.name);
  }

  // A planner message with the Workshop titles it quotes shown plainly.
  std::string tidy(std::string text) const {
    for (const auto &[raw, shown] : coloured_) {
      for (std::size_t pos = text.find(raw); pos != std::string::npos;
           pos = text.find(raw, pos + shown.size())) {
        text.replace(pos, raw.size(), shown);
      }
    }
    return text;
  }

  std::string name_of(const std::string &id) const {
    const xc::installed_content *item = chosen(id);
    return item ? plain(item->info, id) : id;
  }

  std::vector<note_row> notes_for(const std::string &id,
                                  const xc::installed_content *item) const {
    std::vector<note_row> out;
    if (const auto it = notes_.by_id.find(id); it != notes_.by_id.end()) {
      for (const auto &note : it->second) {
        if (note.level != xc::severity::info) {
          out.push_back(
              {tidy(note.message), note.level == xc::severity::error});
        }
      }
    }
    if (item) {
      for (const auto &note :
           xc::mods_folder_notes(owner_->svc().catalog().diags, item->folder)) {
        if (note.level != xc::severity::info) {
          out.push_back(
              {tidy(note.message), note.level == xc::severity::error});
        }
      }
    }
    return out;
  }

  // ----------------------------------------------------------- refresh
  // Compares the services' counters; rebuilds only what changed. The scan
  // bumps catalog_rev for every folder it reads, so progress alone only
  // updates the progress line.
  void sync() {
    services &s = svc();
    const auto &catalog = s.catalog();
    const std::uint64_t key[] = {s.config_rev(), s.game_rev(),
                                 owner_->mode_rev()};
    const bool catalog_changed = s.catalog_rev() != seen_catalog_rev_;
    const bool items_changed = catalog.items.data() != seen_items_ ||
                               catalog.items.size() != seen_count_ ||
                               catalog.scanned != seen_scanned_ ||
                               catalog.scanning != seen_scanning_;
    if (std::equal(std::begin(key), std::end(key), std::begin(seen_)) &&
        !items_changed) {
      if (catalog_changed) {
        seen_catalog_rev_ = s.catalog_rev();
        refresh_scan();
      }
      return;
    }
    std::copy(std::begin(key), std::end(key), std::begin(seen_));
    seen_catalog_rev_ = s.catalog_rev();
    seen_items_ = catalog.items.data();
    seen_count_ = catalog.items.size();
    seen_scanned_ = catalog.scanned;
    seen_scanning_ = catalog.scanning;
    refresh();
  }

  void refresh_scan() {
    const auto &catalog = svc().catalog();
    const auto &game = svc().game();
    scanning_ = catalog.scanning;
    if (catalog.scanning) {
      scan_text_ = catalog.total ? owner_->tr("Reading") + " " +
                                       std::to_string(catalog.done + 1) + "/" +
                                       std::to_string(catalog.total)
                                 : owner_->tr("Looking for mods...");
      scan_detail_ = catalog.current;
    } else if (catalog.scanned) {
      char text[64];
      std::snprintf(text, sizeof(text), "%.1f s", catalog.seconds);
      scan_text_ = std::to_string(catalog.items.size()) + " " +
                   owner_->tr("items") + " \xC2\xB7 " + owner_->tr("read in") +
                   " " + text;
      scan_detail_.clear();
    } else {
      scan_text_ = game.searched && !game.dir ? owner_->tr("No game folder")
                                              : owner_->tr("Not read yet");
      scan_detail_.clear();
    }
    dirty({"scanning", "scan_text", "scan_detail"});
  }

  void refresh() {
    services &s = svc();
    const auto &catalog = s.catalog();
    const auto &game = s.game();
    mode_ = owner_->mode();
    plan_ = s.resolve(mode_);
    search_hint_ = owner_->tr("Search by name, id or Workshop id");
    // While the first scan runs nothing is installed yet as far as the
    // planner knows; "not installed" would be wrong, so notes wait.
    const bool reading = !catalog.scanned && catalog.items.empty();

    // Names for the notes: everything installed and every enabled id. The
    // planner writes raw Workshop titles; the screen shows them without
    // colour codes.
    std::vector<std::pair<std::string, std::string>> names;
    coloured_.clear();
    for (const auto &item : catalog.items) {
      names.emplace_back(item.info.id, item.info.name.empty() ? item.info.id
                                                              : item.info.name);
      if (item.info.name.find('^') != std::string::npos) {
        coloured_.emplace_back(item.info.name,
                               xc::mods_plain_name(item.info.name));
      }
    }
    std::ranges::sort(coloured_, [](const auto &a, const auto &b) {
      return a.first.size() > b.first.size();
    });
    const xc::mode_profile *p = profile();
    if (p) {
      for (const auto &id : p->mods) {
        names.emplace_back(id, id);
      }
    }
    notes_ = reading ? xc::mods_notes{} : xc::mods_attribute(plan_, names);

    // ------------------------------------------------------ load order
    order_.clear();
    const std::vector<std::string> mods =
        p ? p->mods : std::vector<std::string>{};
    for (std::size_t i = 0; i < mods.size(); ++i) {
      const std::string &id = mods[i];
      const xc::installed_content *item = chosen(id);
      order_row row;
      row.key = "o:" + id;
      row.id = id;
      row.n = two_digits(i + 1);
      row.name = item ? plain(item->info, id) : id;
      row.ver = item ? version_text(item->info) : "";
      row.main = i == 0;
      row.first = i == 0;
      row.last = i + 1 == mods.size();
      row.missing = !item && !reading;
      if (!reading) {
        const auto notes = notes_for(id, item);
        row.notes.assign(notes.begin(), notes.end());
      }
      row.problem = std::ranges::any_of(
          row.notes, [](const note_row &n) { return n.error; });
      row.warn = !row.problem && !row.notes.empty();
      if (reading) {
        row.state = owner_->tr("reading");
      } else if (!item) {
        row.state = owner_->tr("not installed");
      } else if (row.problem) {
        row.state = owner_->tr("problem");
      } else if (row.warn) {
        row.state = owner_->tr("warning");
      } else {
        row.state = owner_->tr("on");
      }
      // Dependencies and manifest hints can load a mod somewhere else than
      // where the player put it; say where (only for a plan that resolves).
      for (std::size_t k = 0; k < plan_.stack.size() && plan_.ok(); ++k) {
        if (plan_.stack[k].id == id && k != i) {
          row.loads = owner_->tr("loads") + " " + two_digits(k + 1);
        }
      }
      order_.push_back(std::move(row));
    }
    deps_.clear();
    for (const auto &entry : plan_.stack) {
      if (!entry.requested) {
        deps_.push_back(
            {"o:" + entry.id,
             entry.name.empty() ? entry.id : xc::mods_plain_name(entry.name),
             entry.legacy ? owner_->tr("legacy") : entry.ver.to_string()});
      }
    }
    order_count_ = mods.empty() ? owner_->tr("Vanilla")
                                : std::to_string(mods.size()) + " " +
                                      owner_->tr("enabled");
    order_empty_ =
        owner_->tr("No mods enabled: this mode runs vanilla. Switch mods on "
                   "below; each one goes to the bottom of the order.");
    pack_note_.clear();
    if (p && (!p->pack_name.empty() || !p->pack_id.empty())) {
      pack_note_ = p->pack_name.empty() ? p->pack_id : p->pack_name;
    }
    if (!mods.empty() && !reading) {
      const std::size_t errors = plan_.diags.count(xc::severity::error);
      const std::size_t warnings = plan_.diags.count(xc::severity::warning);
      plan_line_ = owner_->tr("Plan") + " " +
                   (plan_.plan_id.size() > 15 ? plan_.plan_id.substr(7, 8)
                                              : std::string("-")) +
                   " \xC2\xB7 " + std::to_string(plan_.stack.size()) + " " +
                   owner_->tr(plan_.stack.size() == 1 ? "mod" : "mods") +
                   " \xC2\xB7 " + std::to_string(plan_.zones_added) + " " +
                   owner_->tr("zones") + " \xC2\xB7 " + std::to_string(errors) +
                   " " + owner_->tr(errors == 1 ? "problem" : "problems") +
                   " \xC2\xB7 " + std::to_string(warnings) + " " +
                   owner_->tr(warnings == 1 ? "warning" : "warnings");
    } else {
      plan_line_.clear();
    }
    general_.clear();
    for (const auto &note : notes_.general) {
      if (note.level != xc::severity::info) {
        general_.push_back(
            {tidy(note.message), note.level == xc::severity::error});
      }
    }
    for (const auto &item : catalog.diags.items()) {
      if (item.code == "catalog.read") {
        general_.push_back({item.message + " (" + item.path + ")", true});
      }
    }

    // ------------------------------------------------- mods and maps
    all_mods_.clear();
    all_items_.clear();
    for (const xc::installed_content *item : s.mods_for(mode_)) {
      mod_row row;
      row.key = "c:" + std::to_string(index_of(item));
      row.id = item->info.id;
      row.name = plain(item->info, item->info.id);
      row.ver = version_text(item->info);
      row.source = source_of(item);
      row.ws = workshop_of(*item);
      row.legacy = item->info.legacy;
      row.on = enabled(item->info.id);
      // A mod that is off is not in the plan: only its scan notes count.
      const auto notes = row.on ? notes_for(item->info.id, item)
                                : notes_for(std::string(), item);
      row.notes.assign(notes.begin(), notes.end());
      row.problem = !item->valid_manifest ||
                    std::ranges::any_of(
                        row.notes, [](const note_row &n) { return n.error; });
      row.warn = !row.problem && !row.notes.empty();
      if (!item->valid_manifest) {
        row.state = owner_->tr("invalid");
      } else if (row.problem) {
        row.state = owner_->tr("problem");
      } else if (row.on) {
        row.state =
            owner_->tr("on") + " " + two_digits(position(item->info.id));
      } else if (row.warn) {
        row.state = owner_->tr("warning");
      } else {
        row.state = owner_->tr("off");
      }
      all_mods_.push_back(std::move(row));
      all_items_.push_back(item);
    }
    all_maps_.clear();
    map_items_.clear();
    for (const xc::installed_content *item : s.maps()) {
      map_row row;
      row.key = "c:" + std::to_string(index_of(item));
      row.id = item->info.id;
      row.name = plain(item->info, item->info.id);
      row.source = source_of(item);
      if (!item->info.supports_mode(mode_)) {
        row.source += " \xC2\xB7 " + owner_->tr("not for this mode");
      }
      row.on = p && p->map && *p->map == item->info.id;
      all_maps_.push_back(std::move(row));
      map_items_.push_back(item);
    }
    map_none_ = !(p && p->map);
    has_maps_ = !all_maps_.empty();

    // ---------------------------------------------------- empty states
    empty_game_ = false;
    empty_title_.clear();
    empty_text_.clear();
    if (!game.searched) {
      empty_title_ = owner_->tr("Looking for Black Ops III...");
    } else if (!game.dir) {
      empty_game_ = true;
      empty_title_ = owner_->tr("Black Ops III was not found");
      empty_text_ = owner_->tr("Without the game folder there are no mods to "
                               "list. Set it, then rescan.");
    } else if (catalog.items.empty() &&
               (catalog.scanning || !catalog.scanned)) {
      empty_title_ = owner_->tr("Reading your mods...");
      empty_text_ = owner_->tr("The first read hashes every file; later ones "
                               "use the cache.");
    } else if (catalog.items.empty()) {
      empty_title_ = owner_->tr("No mods found");
      empty_text_ = owner_->tr("Xerces looks in the Workshop folder, the "
                               "game's mods folder and its usermaps folder. "
                               "Subscribe to mods in Steam, then rescan.");
    }

    refresh_scan();
    keep_selection();
    apply_filter(); // also rebuilds the details and dirties everything
    can_undo_ = history_.size(mode_) > 0;
    handle_.DirtyAllVariables();
  }

  // The search box and the filter over the cached rows.
  void apply_filter() {
    applied_query_ = query_;
    mods_.clear();
    std::size_t enabled_count = 0, problems = 0, legacy = 0;
    for (std::size_t i = 0; i < all_mods_.size(); ++i) {
      const mod_row &row = all_mods_[i];
      enabled_count += row.on ? 1 : 0;
      problems += row.problem || row.warn ? 1 : 0;
      legacy += row.legacy ? 1 : 0;
      const bool pass = filter_ == "enabled"    ? row.on
                        : filter_ == "problems" ? (row.problem || row.warn)
                        : filter_ == "legacy"   ? row.legacy
                                                : true;
      if (pass && xc::mods_matches(*all_items_[i], query_)) {
        mods_.push_back(row);
      }
    }
    maps_.clear();
    for (std::size_t i = 0; i < all_maps_.size(); ++i) {
      if (xc::mods_matches(*map_items_[i], query_)) {
        maps_.push_back(all_maps_[i]);
      }
    }
    n_all_ = std::to_string(all_mods_.size());
    n_enabled_ = std::to_string(enabled_count);
    n_problems_ = std::to_string(problems);
    n_legacy_ = std::to_string(legacy);
    mods_count_ = (mods_.size() == all_mods_.size()
                       ? std::to_string(all_mods_.size())
                       : std::to_string(mods_.size()) + "/" +
                             std::to_string(all_mods_.size())) +
                  " " + owner_->tr("for") + " " + owner_->tr(mode_name(mode_));
    maps_count_ = std::to_string(maps_.size());
    list_filtered_ = !query_.empty() || filter_ != "all";
    list_empty_.clear();
    if (mods_.empty() && !all_mods_.empty()) {
      list_empty_ = !query_.empty()
                        ? owner_->tr("Nothing matches") + " \"" + query_ + "\"."
                        : owner_->tr("No mods in this filter.");
    } else if (all_mods_.empty() && empty_title_.empty()) {
      list_empty_ = owner_->tr("No installed mod supports") + " " +
                    owner_->tr(mode_name(mode_)) + ".";
    }
    build_details();
    lost_focus_check_ = 2; // rows may have gone; checked after the update
    dirty({"mods", "maps", "n_all", "n_enabled", "n_problems", "n_legacy",
           "mods_count", "maps_count", "list_empty", "list_filtered", "filter",
           "query"});
  }

  // ---------------------------------------------------------- selection
  // Keeps the selected mod across rebuilds (a scan renumbers the catalog);
  // falls back to the first row.
  void keep_selection() {
    const auto &catalog = svc().catalog();
    std::string next;
    if (sel_.section == 'o' && enabled(sel_.id)) {
      next = "o:" + sel_.id;
    } else if (sel_.section == 'o' &&
               std::ranges::any_of(deps_, [&](const dep_row &d) {
                 return d.key == "o:" + sel_.id;
               })) {
      next = "o:" + sel_.id;
    } else if (sel_.section == 'c') {
      for (std::size_t i = 0; i < catalog.items.size(); ++i) {
        if (catalog.items[i].path == sel_.path) {
          next = "c:" + std::to_string(i);
          break;
        }
      }
    } else if (sel_.section == 'n') {
      next = "n:";
    }
    if (next.empty()) {
      next = !order_.empty()      ? order_.front().key
             : !all_mods_.empty() ? all_mods_.front().key
                                  : std::string();
    }
    set_selection(next);
  }

  void set_selection(const std::string &key) {
    sel_key_ = key;
    sel_ = {};
    if (key.size() < 2) {
      return;
    }
    sel_.section = key[0];
    const std::string rest = key.substr(2);
    if (sel_.section == 'o') {
      sel_.id = rest;
      if (const auto *item = chosen(rest)) {
        sel_.path = item->path;
      }
    } else if (sel_.section == 'c') {
      const auto &items = svc().catalog().items;
      const std::size_t i = std::strtoull(rest.c_str(), nullptr, 10);
      if (i < items.size()) {
        sel_.id = items[i].info.id;
        sel_.path = items[i].path;
      } else {
        sel_ = {};
        sel_key_.clear();
      }
    }
  }

  void select(const std::string &key) {
    if (key == sel_key_) {
      return;
    }
    set_selection(key);
    build_details();
    dirty({"sel"});
  }

  const xc::installed_content *selected_item() const {
    if (sel_.section == 'o') {
      return chosen(sel_.id);
    }
    if (sel_.section == 'c') {
      for (const auto &item : owner_->svc().catalog().items) {
        if (item.path == sel_.path) {
          return &item;
        }
      }
    }
    return nullptr;
  }

  // ------------------------------------------------------------ details
  void build_details() {
    d_fields_.clear();
    d_notes_.clear();
    d_fixes_.clear();
    d_fix_text_.clear();
    d_why_.clear();
    d_caps_.clear();
    d_settings_.clear();
    sel_fixes_.clear();
    d_has_ = !sel_.id.empty();
    d_ws_.clear();
    d_path_.clear();
    d_legacy_ = false;
    d_can_toggle_ = false;
    if (!d_has_) {
      dirty_details();
      return;
    }
    const xc::installed_content *item = selected_item();
    const std::string &id = sel_.id;
    const bool on = enabled(id);
    const auto field = [&](const std::string &label, const std::string &value) {
      if (!value.empty()) {
        d_fields_.push_back({owner_->tr(label), value});
      }
    };
    d_id_ = id;
    if (!item) {
      d_kind_ = owner_->tr("Mod") + " \xC2\xB7 " + owner_->tr("not installed");
      d_name_ = id;
      d_can_toggle_ = on;
      d_toggle_ = owner_->tr("Remove from the load order");
      if (id.starts_with("ws.")) {
        d_ws_ = id.substr(3);
      }
      if (on) {
        field("Load order", two_digits(position(id)) + " " + owner_->tr("of") +
                                " " + std::to_string(profile()->mods.size()));
      }
    } else {
      const xc::manifest &info = item->info;
      const bool is_map = info.kind == xc::content_kind::map;
      d_kind_ = owner_->tr(is_map ? "Map" : "Mod");
      if (const std::string source = source_of(item); !source.empty()) {
        d_kind_ += " \xC2\xB7 " + source; // already translated
      }
      if (info.legacy) {
        d_kind_ += " \xC2\xB7 " + owner_->tr("legacy");
      }
      d_name_ = plain(info, id);
      d_ws_ = workshop_of(*item);
      d_path_ = item->path.generic_string();
      d_legacy_ = info.legacy;
      if (is_map) {
        const xc::mode_profile *p = profile();
        const bool picked = p && p->map && *p->map == id;
        d_can_toggle_ = true;
        d_toggle_ =
            picked ? owner_->tr("Use no map") : owner_->tr("Use this map");
      } else {
        d_can_toggle_ = on || info.supports_mode(mode_);
        d_toggle_ = on ? owner_->tr("Switch off") : owner_->tr("Switch on");
      }

      field("Version", info.legacy ? owner_->tr("legacy") +
                                         (info.ver.build.empty()
                                              ? ""
                                              : " (" + info.ver.build + ")")
                                   : info.ver.to_string());
      if (!is_map) {
        std::string place = owner_->tr("Off");
        if (on) {
          place =
              two_digits(position(id)) + " " + owner_->tr("of") + " " +
              std::to_string(profile()->mods.size()) +
              (position(id) == 1 ? " \xC2\xB7 " + owner_->tr("main mod") : "");
        } else {
          for (std::size_t k = 0; k < plan_.stack.size(); ++k) {
            if (plan_.stack[k].id == id) {
              place = owner_->tr("Loads at") + " " + two_digits(k + 1) +
                      " \xC2\xB7 " + owner_->tr("added as a dependency");
            }
          }
        }
        field("Load order", place);
      }
      field("Authors", join(info.authors));
      field("License", info.license);
      std::vector<std::string> modes;
      for (const auto &mode : info.modes) {
        modes.push_back(owner_->tr(mode == "zm"   ? "Zombies"
                                   : mode == "mp" ? "Multiplayer"
                                   : mode == "cp" ? "Campaign"
                                                  : mode.c_str()));
      }
      field("Modes", modes.empty() ? owner_->tr("Every mode") : join(modes));
      field("Workshop id", d_ws_);
      field("Folder", d_path_);
      field("Files", std::to_string(item->files.size()));
      field("Zones", std::to_string(xc::count_zone_files(item->files)));
      field("Content hash", xc::mods_short_hash(item->content_hash));
      if (const xc::mode_profile *p = profile()) {
        if (const auto pin = p->pins.find(id); pin != p->pins.end()) {
          field("Pinned", pin->second);
        }
      }
      if (!info.legacy) {
        field("Touches", std::to_string(info.provides.size()) + " " +
                             owner_->tr("provided") + " \xC2\xB7 " +
                             std::to_string(info.overrides.size()) + " " +
                             owner_->tr("overridden") + " \xC2\xB7 " +
                             std::to_string(info.extends.size()) + " " +
                             owner_->tr("extended") + " \xC2\xB7 " +
                             std::to_string(info.hooks.size()) + " " +
                             owner_->tr("hooks"));
        std::vector<std::string> needs;
        for (const auto &dep : info.dependencies) {
          needs.push_back(
              dep.id + " " + dep.range_text +
              (dep.optional ? " (" + owner_->tr("optional") + ")" : ""));
        }
        field("Needs", join(needs));
        std::vector<std::string> conflicts;
        for (const auto &c : info.conflicts) {
          conflicts.push_back(c.id);
        }
        field("Conflicts with", join(conflicts));
      }
      for (const auto &cap : xc::mods_capabilities(info.caps)) {
        d_caps_.push_back({owner_->tr(cap.label), cap.detail});
      }
      for (const auto &setting : info.settings) {
        d_settings_.push_back(
            {setting.label.empty() ? setting.key : setting.label,
             setting.type + " \xC2\xB7 " + owner_->tr("default") + " " +
                 setting.default_json +
                 (setting.dvar.empty() ? "" : " \xC2\xB7 " + setting.dvar)});
      }
    }
    const std::string unknown =
        !item ? owner_->tr("Unknown: it is not installed.")
        : item->info.legacy
            ? owner_->tr("Unknown: legacy mods declare nothing.")
            : owner_->tr("None declared.");
    d_caps_empty_ = unknown;
    d_settings_empty_ = unknown;

    // Problems for this mod, with the planner's fixes.
    for (const auto &note : notes_for(id, item)) {
      d_notes_.push_back(note);
    }
    if (item && !item->valid_manifest) {
      d_notes_.push_back(
          {owner_->tr("Its xerces.mod.json has errors; the plan will not "
                      "load it."),
           true});
    }
    const bool is_plan_map = plan_.map && plan_.map->id == id;
    if (on || is_plan_map) {
      for (const auto &fix : plan_.fixes) {
        if (fix.id == id) {
          add_fix(fix);
        }
      }
    }
    // The planner files a missing, pinned or outdated dependency under the
    // dependency's id, which has no row of its own; the mod that needs it
    // offers the fix. Also for a mod that is only in the plan as another
    // mod's dependency.
    const bool in_plan =
        on || is_plan_map ||
        std::ranges::any_of(
            plan_.stack, [&](const xc::plan_entry &e) { return e.id == id; });
    if (item && in_plan) {
      for (const auto &dep : item->info.dependencies) {
        for (const auto &fix : plan_.fixes) {
          if (fix.id != dep.id || fix.id == id) {
            continue;
          }
          if (fix.kind == "install" || fix.kind == "update") {
            // The planner gives one range for every mod that asks; this
            // mod's own is the one to show here.
            add_fix({fix.kind, fix.id, dep.range_text});
          } else if (fix.kind == "unpin") {
            add_fix(fix);
          }
        }
      }
    }
    // Why this order: the planner's decisions that name this mod.
    for (const auto &why : plan_.explanations) {
      if ((why.subject == id || why.object == id) && d_why_.size() < 8) {
        d_why_.push_back(why.message);
      }
    }
    dirty_details();
  }

  // A fix the screen can apply becomes a button; the others are a line.
  void add_fix(const xc::fix_action &fix) {
    std::string label;
    if (fix.kind == "remove") {
      label = owner_->tr("Remove from the load order");
    } else if (fix.kind == "reorder" && enabled(fix.detail)) {
      label = owner_->tr("Move below") + " " + name_of(fix.detail);
    } else if (fix.kind == "unpin") {
      // A dependency's pin names the dependency; the mod's own does not.
      label =
          owner_->tr("Unpin version") + " " + fix.detail +
          (fix.id != sel_.id ? " " + owner_->tr("of") + " " + name_of(fix.id)
                             : std::string());
    } else if ((fix.kind == "install" || fix.kind == "update") &&
               fix.id.starts_with("ws.") && fix.id.substr(3) != d_ws_) {
      // A dependency from the Workshop (the selected mod's own page is
      // the Open in Steam button above). Its id tells two of them apart.
      label = owner_->tr("Open in Steam:") + " " + fix.id.substr(3);
    }
    if (!label.empty()) {
      for (const auto &existing : sel_fixes_) {
        if (existing.kind == fix.kind && existing.id == fix.id &&
            existing.detail == fix.detail) {
          return;
        }
      }
      d_fixes_.push_back({label, int(sel_fixes_.size())});
      sel_fixes_.push_back(fix);
      return;
    }
    std::string text;
    if (fix.kind == "allow_conflict") {
      text = owner_->tr("A pack can allow this conflict (Packs).");
    } else if (fix.kind == "update_xerces") {
      text = owner_->tr("Needs Xerces") + " " + fix.detail;
    } else if (fix.kind == "install" || fix.kind == "update") {
      text = fix.id.starts_with("ws.")
                 ? owner_->tr("Subscribe to it in Steam (Open in Steam), then "
                              "rescan.")
                 : owner_->tr("Install") + " " + fix.id +
                       (fix.detail.empty() ? "" : " " + fix.detail) + ", " +
                       owner_->tr("then rescan.");
    }
    if (!text.empty() &&
        std::ranges::find(d_fix_text_, text) == d_fix_text_.end()) {
      d_fix_text_.push_back(text);
    }
  }

  void dirty_details() {
    dirty({"d_has", "d_kind", "d_name", "d_id", "d_ws", "d_path", "d_legacy",
           "d_toggle", "d_can_toggle", "d_fields", "d_notes", "d_fixes",
           "d_fix_text", "d_why", "d_caps", "d_caps_empty", "d_settings",
           "d_settings_empty", "sel"});
  }

  // -------------------------------------------------------------- edits
  // One hand edit: apply, detach from the pack, record for Ctrl+Z, save.
  // message runs after the change, so it can say where a mod ended up.
  template <typename Edit, typename Message>
  bool edit(Edit &&apply, Message &&message) {
    auto &config = svc().config();
    const xc::mods_snapshot before = xc::mods_take(config, mode_);
    if (!apply(xc::profile_for(config, mode_))) {
      return false;
    }
    const std::string done = message();
    const std::string detached = xc::mods_hand_edit(config, mode_);
    history_.record(mode_, before, xc::mods_take(config, mode_));
    if (!svc().save_config()) {
      notify(owner_->tr("The config could not be saved:") + " " +
                 svc().config_error(),
             true);
    } else if (!detached.empty()) {
      notify(done + ". " + owner_->tr("Detached from the pack") + " \"" +
             detached + "\" (" + owner_->tr("Ctrl+Z undoes") + ").");
    } else {
      notify(done);
    }
    log_info("launcher.mods.edit", done);
    return true;
  }

  void toggle(const std::string &id, const Rml::Element *from) {
    if (id.empty()) {
      return;
    }
    const bool on = enabled(id);
    const std::string name = name_of(id);
    const std::size_t was = position(id);
    const std::string from_id = from ? from->GetId() : std::string();
    const bool done = edit(
        [&](xc::mode_profile &p) { return xc::mods_set_enabled(p, id, !on); },
        [&] {
          if (on) {
            return name + " " + owner_->tr("is off");
          }
          const std::size_t at = position(id);
          return name + " " + owner_->tr("is on at") + " " + two_digits(at) +
                 (at == 1 ? " (" + owner_->tr("main mod") + ")"
                          : " (" + owner_->tr("the bottom") + ")");
        });
    if (!done) {
      return;
    }
    // Keep the keyboard where it was: switching off in the load order
    // removes that row, so focus the row that took its place.
    if (from_id.starts_with("ot-") || from_id.starts_with("om-")) {
      const std::size_t count = profile() ? profile()->mods.size() : 0;
      if (count == 0) {
        focus_later("lt-0");
      } else {
        const std::size_t row = std::min(was - 1, count - 1);
        focus_later(from_id.substr(0, 3) + std::to_string(row));
        set_selection("o:" + profile()->mods[row]);
      }
    } else if (!from_id.empty()) {
      focus_later(from_id);
    }
  }

  // Moves the mod at load-order index `index` by delta places; prefix is the
  // control to keep focused ("ou-", "od-", "om-").
  void move_index(int index, int delta, const std::string &prefix) {
    const xc::mode_profile *p = profile();
    if (!p || index < 0 || std::size_t(index) >= p->mods.size()) {
      return;
    }
    const std::string id = p->mods[std::size_t(index)];
    move_id(id, delta, prefix);
  }

  void move_id(const std::string &id, int delta, const std::string &prefix) {
    const std::string name = name_of(id);
    const bool done = edit(
        [&](xc::mode_profile &p) { return xc::mods_move_by(p, id, delta); },
        [&] {
          const std::size_t at = position(id);
          return name + " " + owner_->tr("moved to") + " " + two_digits(at) +
                 (at == 1 ? " (" + owner_->tr("main mod") + ")" : "");
        });
    if (!done) {
      notify(name + " " +
             owner_->tr(delta < 0 ? "is already at the top"
                                  : "is already at the bottom"));
      return;
    }
    set_selection("o:" + id);
    if (!prefix.empty()) {
      focus_later(prefix + std::to_string(position(id) - 1));
    }
  }

  void move_selected(int delta) {
    if (sel_.id.empty()) {
      return;
    }
    if (!enabled(sel_.id)) {
      notify(owner_->tr("Switch it on first: only enabled mods have a "
                        "place in the load order."),
             true);
      return;
    }
    const Rml::Element *focus = owner_->context()->GetFocusElement();
    const std::string focus_id = focus ? focus->GetId() : std::string();
    // From a load-order row, focus follows the mod; elsewhere it stays.
    const std::string prefix =
        focus_id.size() > 3 && focus_id[0] == 'o' && focus_id[2] == '-'
            ? focus_id.substr(0, 3)
            : std::string();
    move_id(sel_.id, delta, prefix);
  }

  void pick_map(const std::string &id) {
    const std::string name = id.empty() ? owner_->tr("No map") : name_of(id);
    edit(
        [&](xc::mode_profile &p) {
          return xc::mods_set_map(
              p, id.empty() ? std::nullopt : std::optional<std::string>(id));
        },
        [&] {
          return id.empty() ? owner_->tr("No map for") + " " +
                                  owner_->tr(mode_name(mode_))
                            : name + " " + owner_->tr("is the map for") + " " +
                                  owner_->tr(mode_name(mode_));
        });
  }

  void undo() {
    auto &config = svc().config();
    switch (history_.undo(config, mode_)) {
    case xc::mods_history::undo_result::done:
      if (!svc().save_config()) {
        notify(owner_->tr("The config could not be saved:") + " " +
                   svc().config_error(),
               true);
      } else {
        notify(owner_->tr("Undone"));
      }
      log_info("launcher.mods.undo", mode_);
      break;
    case xc::mods_history::undo_result::stale:
      notify(owner_->tr("Nothing to undo: the load order was changed "
                        "elsewhere since."),
             true);
      break;
    case xc::mods_history::undo_result::nothing:
      notify(owner_->tr("Nothing to undo"));
      break;
    }
    can_undo_ = history_.size(mode_) > 0;
    dirty({"can_undo"});
  }

  void apply_fix(int index) {
    if (index < 0 || std::size_t(index) >= sel_fixes_.size()) {
      return;
    }
    const xc::fix_action fix = sel_fixes_[std::size_t(index)];
    const std::string name = name_of(fix.id);
    if (fix.kind == "remove") {
      if (profile() && profile()->map == fix.id) {
        pick_map("");
      } else {
        toggle(fix.id, nullptr);
      }
    } else if (fix.kind == "reorder") {
      // The planner wants fix.id below fix.detail (it is a dependency).
      edit(
          [&](xc::mode_profile &p) {
            const auto from = std::ranges::find(p.mods, fix.id);
            const auto to = std::ranges::find(p.mods, fix.detail);
            if (from == p.mods.end() || to == p.mods.end() || from > to) {
              return false;
            }
            return xc::mods_move(p, std::size_t(from - p.mods.begin()),
                                 std::size_t(to - p.mods.begin()));
          },
          [&] {
            return name + " " + owner_->tr("moved below") + " " +
                   name_of(fix.detail);
          });
    } else if (fix.kind == "unpin") {
      edit([&](xc::mode_profile &p) { return xc::mods_unpin(p, fix.id); },
           [&] { return name + " " + owner_->tr("is no longer pinned"); });
    } else if (fix.kind == "install" || fix.kind == "update") {
      open_steam(fix.id.substr(3));
    }
  }

  void open_steam(const std::string &workshop_id) {
    if (workshop_id.empty() ||
        workshop_id.find_first_not_of("0123456789") != std::string::npos) {
      return;
    }
    if (!owner_->open_url("steam://url/CommunityFilePage/" + workshop_id)) {
      notify(owner_->tr("Steam could not be opened."), true);
    }
  }

  void act(const std::string &action) {
    log_info("launcher.mods.action", action);
    if (action == "rescan") {
      if (svc().catalog().scanning) {
        return;
      }
      svc().start_scan();
      notify(owner_->tr("Reading the mod folders again..."));
    } else if (action == "cancel") {
      svc().cancel_scan();
    } else if (action == "undo") {
      undo();
    } else if (action == "toggle") {
      const xc::installed_content *item = selected_item();
      if (item && item->info.kind == xc::content_kind::map) {
        const xc::mode_profile *p = profile();
        pick_map(p && p->map && *p->map == sel_.id ? "" : sel_.id);
      } else {
        toggle(sel_.id, nullptr);
      }
    } else if (action == "steam") {
      open_steam(d_ws_);
    } else if (action == "folder") {
      if (const xc::installed_content *item = selected_item()) {
        if (!owner_->open_folder(item->path)) {
          notify(owner_->tr("The folder could not be opened."), true);
        }
      }
    } else if (action == "copy id") {
      owner_->copy_text(sel_.id);
      notify(owner_->tr("Copied") + " " + sel_.id);
    } else if (action == "clear") {
      query_.clear();
      filter_ = "all";
      apply_filter();
      focus_later("search");
    } else if (action == "find game") {
      owner_->show(owner_->find("first_run") ? "first_run" : "settings");
    }
  }

  // ------------------------------------------------------- navigation
  // RmlUi's spatial navigation never leaves the scroll container it starts
  // in, and never enters one, so the list and the details pane would trap a
  // controller. The screen has three areas (the top bar, the list, the
  // details); moves inside the top bar and the list are RmlUi's, moves
  // between areas, inside the details (which also page through their text)
  // and from plain text a click focused are made here with the same scoring
  // RmlUi uses.
  struct area_box {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  };

  static area_box box_of(Rml::Element *element) {
    const Rml::Vector2f at = element->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
    return {at.x, at.y, at.x + size.x, at.y + size.y};
  }

  // Distance along the move plus a large penalty for being off to the side
  // (as in RmlUi's GetNavigationHeuristic); negative: not that way at all.
  static float score(const area_box &from, const area_box &to,
                     Rml::Input::KeyIdentifier key) {
    float main = 0, cross = 0;
    switch (key) {
    case Rml::Input::KI_UP:
      main = from.y0 - to.y1;
      cross = std::max(0.f, to.x0 - from.x1) + std::max(0.f, from.x0 - to.x1);
      break;
    case Rml::Input::KI_DOWN:
      main = to.y0 - from.y1;
      cross = std::max(0.f, to.x0 - from.x1) + std::max(0.f, from.x0 - to.x1);
      break;
    case Rml::Input::KI_LEFT:
      main = from.x0 - to.x1;
      cross = std::max(0.f, to.y0 - from.y1) + std::max(0.f, from.y0 - to.y1);
      break;
    default:
      main = to.x0 - from.x1;
      cross = std::max(0.f, to.y0 - from.y1) + std::max(0.f, from.y0 - to.y1);
      break;
    }
    return main < 0 ? -1.f : main + 10000.f * cross;
  }

  // 0 the top bar, 1 the list, 2 the details, -1 elsewhere (tabs, footer).
  static int area_of(const Rml::Element *element) {
    for (; element; element = element->GetParentNode()) {
      const Rml::String &name = element->GetId();
      if (name == "mods-top") {
        return 0;
      }
      if (name == "mods-list") {
        return 1;
      }
      if (name == "mods-details") {
        return 2;
      }
    }
    return -1;
  }

  // Something RmlUi can navigate from and Enter acts on: the buttons, the
  // search box and the details' section headings (tab-index: auto). A click
  // on plain text focuses that div instead, and RmlUi cannot move from it.
  static bool is_stop(const Rml::Element *element) {
    return element->GetComputedValues().tab_index() ==
           Rml::Style::TabIndex::Auto;
  }

  // The best target in one area. Moving into the list or the details, only
  // what is scrolled into view counts; inside one, RmlUi scrolls to it.
  Rml::Element *best_in(int area, Rml::Element *from,
                        Rml::Input::KeyIdentifier key, float &best,
                        bool in_view) const {
    static const char *roots[] = {"mods-top", "mods-list", "mods-details"};
    Rml::Element *root = doc ? doc->GetElementById(roots[area]) : nullptr;
    if (!root) {
      return nullptr;
    }
    const area_box clip = box_of(root);
    const area_box start = box_of(from);
    Rml::Element *found = nullptr;
    for (const char *tag : {"button", "input", ".d-stop"}) {
      Rml::ElementList elements;
      if (tag[0] == '.') {
        root->GetElementsByClassName(elements, tag + 1);
      } else {
        root->GetElementsByTagName(elements, tag);
      }
      for (Rml::Element *element : elements) {
        if (element == from || !element->IsVisible(true)) {
          continue;
        }
        const area_box b = box_of(element);
        if (b.x1 - b.x0 <= 0 || b.y1 - b.y0 <= 0) {
          continue;
        }
        if (in_view && area != 0 && (b.y1 <= clip.y0 || b.y0 >= clip.y1)) {
          continue; // scrolled out of view
        }
        const float s = score(start, b, key);
        if (s >= 0 && s < best) {
          best = s;
          found = element;
        }
      }
    }
    return found;
  }

  // The selected mod's row in the list (where Left from the details goes).
  std::string selected_row_id() const {
    for (std::size_t i = 0; i < order_.size(); ++i) {
      if (order_[i].key == sel_key_) {
        return "om-" + std::to_string(i);
      }
    }
    for (std::size_t i = 0; i < deps_.size(); ++i) {
      if (deps_[i].key == sel_key_) {
        return "dp-" + std::to_string(i);
      }
    }
    for (std::size_t i = 0; i < mods_.size(); ++i) {
      if (mods_[i].key == sel_key_) {
        return "lm-" + std::to_string(i);
      }
    }
    for (std::size_t i = 0; i < maps_.size(); ++i) {
      if (maps_[i].key == sel_key_) {
        return "mp-" + std::to_string(i);
      }
    }
    return sel_key_ == "n:" ? "map-none" : "";
  }

  // True when the move was made here; false leaves it to RmlUi.
  bool navigate(Rml::Element *from, Rml::Input::KeyIdentifier key) {
    const int area = area_of(from);
    if (!doc) {
      return false;
    }
    if (area < 0) {
      // From the header's tabs, Down enters the screen (the content is a
      // scroll container, which RmlUi's navigation does not enter).
      bool in_header = false;
      for (const Rml::Element *e = from; e; e = e->GetParentNode()) {
        in_header = in_header || e->GetId() == "header";
      }
      float best = 1e30f;
      Rml::Element *target = in_header && key == Rml::Input::KI_DOWN
                                 ? best_in(0, from, key, best, true)
                                 : nullptr;
      if (target) {
        target->Focus(true);
      }
      return target != nullptr;
    }
    const bool text = from->GetTagName() == "input";
    if (text && (key == Rml::Input::KI_LEFT || key == Rml::Input::KI_RIGHT)) {
      return false; // the caret
    }
    const bool vertical =
        key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN;
    Rml::Element *target = nullptr;
    if (area == 2 && key == Rml::Input::KI_LEFT) {
      target = doc->GetElementById(selected_row_id());
      if (target && !target->IsVisible(true)) {
        target = nullptr;
      }
    }
    if (!target) {
      float best = 1e30f;
      // The search box would keep Up and Down for itself, so even moves
      // inside the top bar are made here when it has focus.
      Rml::Element *inside =
          text ? nullptr : best_in(area, from, key, best, false);
      if (area == 2 && vertical && page_details(inside, key)) {
        return true; // read on before the next stop
      }
      if (inside && area != 2 && is_stop(from)) {
        return false; // RmlUi finds it
      }
      // From plain text RmlUi finds nothing; the details are paged here.
      target = inside;
    }
    if (!target) {
      // Between areas only the ways that read naturally: the top bar and
      // the columns by Up and Down, the list and the details by Left and
      // Right (Down at the end of the details does not jump to the list).
      float best = 1e30f;
      for (int other = 0; other < 3; ++other) {
        const bool allowed = other == area
                                 ? text
                                 : (vertical ? (area == 0) != (other == 0)
                                             : area != 0 && other != 0);
        if (allowed) {
          if (Rml::Element *e = best_in(other, from, key, best, true)) {
            target = e;
          }
        }
      }
    }
    if (!target && area == 0 && key == Rml::Input::KI_UP) {
      target = doc->GetElementById("tab-" + std::string(id()));
    }
    if (!target && !is_stop(from)) {
      // Focus sits on plain text (after a click) and nothing lies that way:
      // the selected row, else the screen's default, so the keys never go
      // dead.
      target = doc->GetElementById(selected_row_id());
      if (!target || !target->IsVisible(true)) {
        target = default_focus();
      }
    }
    if (!target || target == from) {
      return false;
    }
    target->Focus(true);
    target->ScrollIntoView(
        Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
    return true;
  }

  // RmlUi has no keyboard scrolling and the details are mostly text, so Up
  // and Down page through them (80% of the pane, a little overlap to keep
  // the place) whenever the next stop that way is further than one step
  // can bring into view along with what lies before it, or there is none.
  // True when it scrolled.
  bool page_details(Rml::Element *next, Rml::Input::KeyIdentifier key) {
    Rml::Element *pane = doc ? doc->GetElementById("mods-details") : nullptr;
    if (!pane) {
      return false;
    }
    const float client = pane->GetClientHeight();
    const float page = client * 0.8f;
    const bool down = key == Rml::Input::KI_DOWN;
    if (next) {
      const area_box view = box_of(pane);
      const area_box b = box_of(next);
      if (down ? b.y1 <= view.y1 + page : b.y0 >= view.y0 - page) {
        return false; // focusing it scrolls nothing unread past
      }
    }
    const float top = pane->GetScrollTop();
    const float end = std::max(0.f, pane->GetScrollHeight() - client);
    const float to = std::clamp(top + (down ? page : -page), 0.f, end);
    if (std::fabs(to - top) < 1.f) {
      return false; // already at that end
    }
    pane->SetScrollTop(to);
    return true;
  }

  // The load order's first row, else the list's; else the base rule (the
  // first "default-focus", then the first button). RmlUi's class lookup is
  // breadth first, which would pick the list before the load order.
  Rml::Element *default_focus() override {
    if (doc) {
      for (const char *name : {"om-0", "lm-0"}) {
        if (Rml::Element *element = doc->GetElementById(name);
            element && element->IsVisible(true)) {
          return element;
        }
      }
    }
    return view::default_focus();
  }

  // ------------------------------------------------------------ focus
  // Shown before the first scan finished, the screen had nothing but the
  // mode switch or Rescan to focus; once rows exist the keyboard belongs on
  // them. Only once, and only from those fallbacks, so it never pulls focus
  // away from something the player picked.
  void first_focus() {
    if (had_rows_ || owner_->current() != id() || !doc) {
      return;
    }
    if (order_.empty() && all_mods_.empty()) {
      return;
    }
    had_rows_ = true;
    const Rml::Element *focus =
        owner_->context() ? owner_->context()->GetFocusElement() : nullptr;
    const std::string focus_id = focus ? focus->GetId() : std::string();
    if (!focus || focus == doc || focus_id.starts_with("mode-") ||
        focus_id == "rescan" || focus_id == "cancel-scan" ||
        focus_id == "empty-rescan") {
      focus_later("");
    }
  }

  // Focus must stay on something with a ring. A rebuild can remove the
  // focused row (another mode, a rescan, a filter), and a control that
  // data-if hides (Undo once the history is empty, Rescan and Stop
  // swapping) is blurred by RmlUi onto its parent: either way focus ends on
  // a plain div. A hidden control hands focus to its neighbour, a removed
  // row to the screen's default. A plain div focused by a click on text is
  // left alone (navigate() moves on from it).
  void keep_focus_visible() {
    const bool rebuilt = lost_focus_check_ > 0 && --lost_focus_check_ == 0;
    Rml::Element *focus =
        owner_->context() ? owner_->context()->GetFocusElement() : nullptr;
    if (!focus || !doc || !doc->IsVisible() || owner_->current() != id() ||
        owner_->dialog_open() || area_of(focus) < 0) {
      return;
    }
    if (is_stop(focus) && focus->IsVisible(true)) {
      last_stop_ = focus->GetId();
      return;
    }
    Rml::Element *was =
        last_stop_.empty() ? nullptr : doc->GetElementById(last_stop_);
    bool blurred = false; // focus went from `was` to a parent of it
    if (was && !was->IsVisible(true)) {
      for (const Rml::Element *e = was; e && !blurred; e = e->GetParentNode()) {
        blurred = e == focus;
      }
    }
    if (blurred) {
      // Remembered at once: a cached rescan can end (Stop hides again)
      // before a frame passes with Stop focused.
      last_stop_ = last_stop_ == "rescan"        ? "cancel-scan"
                   : last_stop_ == "cancel-scan" ? "rescan"
                   : last_stop_ == "undo"        ? "filter-legacy"
                                                 : "";
      focus_now(last_stop_);
    } else if (rebuilt || !focus->IsVisible(true)) {
      focus_now("");
    }
  }

  // After a change the rows are rebuilt at the next context update, so focus
  // waits a frame. An empty id means the screen's default focus.
  void focus_later(const std::string &element_id) {
    pending_focus_ = element_id.empty() ? std::string("*") : element_id;
    pending_wait_ = 1;
  }

  void focus_now(const std::string &element_id) {
    pending_focus_ = element_id.empty() ? std::string("*") : element_id;
    apply_pending_focus();
  }

  void apply_pending_focus() {
    const std::string target = std::move(pending_focus_);
    pending_focus_.clear();
    if (!doc || owner_->current() != id() || owner_->dialog_open()) {
      return;
    }
    Rml::Element *element =
        target == "*" ? nullptr : doc->GetElementById(target);
    if (!element || !element->IsVisible(true)) {
      element = default_focus();
    }
    if (element) {
      element->Focus(true);
      element->ScrollIntoView(
          Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
    }
  }

  app *owner_ = nullptr;
  Rml::DataModelHandle handle_;
  std::uint64_t seen_[3] = {};
  std::uint64_t seen_catalog_rev_ = 0;
  const void *seen_items_ = nullptr;
  std::size_t seen_count_ = 0;
  bool seen_scanned_ = false, seen_scanning_ = false;

  xc::plan plan_;
  xc::mods_notes notes_;
  xc::mods_history history_;
  selection sel_;
  std::vector<xc::fix_action> sel_fixes_;
  std::vector<mod_row> all_mods_;
  std::vector<const xc::installed_content *> all_items_;
  std::vector<map_row> all_maps_;
  std::vector<const xc::installed_content *> map_items_;
  std::vector<std::pair<std::string, std::string>> coloured_; // raw, plain
  bool had_rows_ = false;
  int lost_focus_check_ = 0;
  std::string last_stop_; // the last control with focus, by id
  std::string applied_query_;
  std::string pending_focus_;
  int pending_wait_ = 0;

  Rml::String mode_ = "zm", query_, search_hint_, filter_ = "all";
  Rml::String n_all_, n_enabled_, n_problems_, n_legacy_;
  bool scanning_ = false;
  Rml::String scan_text_, scan_detail_, empty_title_, empty_text_;
  bool empty_game_ = false;
  Rml::Vector<order_row> order_;
  Rml::String order_count_, order_empty_, pack_note_, plan_line_;
  Rml::Vector<note_row> general_;
  Rml::Vector<dep_row> deps_;
  Rml::Vector<mod_row> mods_;
  Rml::String mods_count_, list_empty_;
  bool list_filtered_ = false;
  Rml::Vector<map_row> maps_;
  Rml::String maps_count_;
  bool map_none_ = true, has_maps_ = false;
  Rml::String sel_key_;
  int drag_from_ = -1, drop_at_ = -1;
  bool can_undo_ = false;

  bool d_has_ = false;
  Rml::String d_kind_, d_name_, d_id_, d_ws_, d_path_, d_toggle_;
  bool d_legacy_ = false, d_can_toggle_ = false;
  Rml::Vector<field_row> d_fields_;
  Rml::Vector<note_row> d_notes_;
  Rml::Vector<fix_row> d_fixes_;
  Rml::Vector<Rml::String> d_fix_text_, d_why_;
  Rml::Vector<field_row> d_caps_, d_settings_;
  Rml::String d_caps_empty_, d_settings_empty_;
};
} // namespace

XERCES_VIEW(mods_view, "mods", 20, true);
} // namespace xerces_launcher
