// Packs (docs/DESIGN.md 7.3, 8.1 to 8.3): the pack library in
// <home>\config\packs, saving the current mode's plan as a pack, applying a
// pack to its mode's profile (backup first, then the check of what is
// installed), exporting to a file or a share code, importing either, and
// deleting. The file work and the checks live in xerces_core/packs_store so
// they are unit-tested; this file is the screen.

#include <RmlUi/Core.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>

#include <xerces_core/backup.hpp>
#include <xerces_core/pack.hpp>
#include <xerces_core/packs_store.hpp>

#include "../app.hpp"
#include "../log.hpp"

namespace xerces_launcher {
namespace {
namespace core = xerces_core;

struct pack_row {
  Rml::String id, name, meta, state;
  bool bad = false, selected = false;
  bool operator==(const pack_row &) const = default;
};

struct item_row {
  Rml::String n, name, sub, ver, state, ws;
  bool bad = false;
  bool operator==(const item_row &) const = default;
};

const char *mode_name(const std::string &mode) {
  return mode == "mp" ? "Multiplayer" : mode == "cp" ? "Campaign" : "Zombies";
}

std::string two_digits(std::size_t value) {
  char text[16];
  std::snprintf(text, sizeof(text), "%02zu", value);
  return text;
}

std::string utf8(const fs::path &path) {
  const auto text = path.u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

fs::path from_utf8(const std::string &text) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()),
                                text.size()));
}

// Backups are named by UTC time, as the other backups are (8.5).
std::string backup_stamp() {
  const std::time_t t = std::time(nullptr);
  std::tm utc{};
  gmtime_s(&utc, &t);
  char text[32];
  std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &utc);
  return text;
}

std::string quoted(const std::string &text) { return "\"" + text + "\""; }

std::string short_id(const std::string &id) { return id.substr(0, 8); }

bool write_file(const fs::path &file, const std::string &text) {
  fs::path temporary = file;
  temporary += ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out.flush()) {
      return false;
    }
  }
  std::error_code ec;
  fs::rename(temporary, file, ec);
  if (ec) {
    fs::remove(temporary, ec);
  }
  return !ec;
}

class packs_view final : public view {
public:
  const char *id() const override { return "packs"; }

  bool bind(Rml::Context *context, app &owner) override {
    owner_ = &owner;
    Rml::DataModelConstructor model = context->CreateDataModel("packs");
    if (!model) {
      return false;
    }
    if (auto row = model.RegisterStruct<pack_row>()) {
      row.RegisterMember("id", &pack_row::id);
      row.RegisterMember("name", &pack_row::name);
      row.RegisterMember("meta", &pack_row::meta);
      row.RegisterMember("state", &pack_row::state);
      row.RegisterMember("bad", &pack_row::bad);
      row.RegisterMember("selected", &pack_row::selected);
    }
    if (auto row = model.RegisterStruct<item_row>()) {
      row.RegisterMember("n", &item_row::n);
      row.RegisterMember("name", &item_row::name);
      row.RegisterMember("sub", &item_row::sub);
      row.RegisterMember("ver", &item_row::ver);
      row.RegisterMember("state", &item_row::state);
      row.RegisterMember("ws", &item_row::ws);
      row.RegisterMember("bad", &item_row::bad);
    }
    model.RegisterArray<Rml::Vector<pack_row>>();
    model.RegisterArray<Rml::Vector<item_row>>();
    model.RegisterArray<Rml::Vector<Rml::String>>();

    model.Bind("packs", &rows_);
    model.Bind("count_text", &count_text_);
    model.Bind("empty_text", &empty_text_);
    model.Bind("library_note", &library_note_);
    model.Bind("has_sel", &has_sel_);
    model.Bind("sel_name", &sel_name_);
    model.Bind("sel_meta", &sel_meta_);
    model.Bind("sel_state", &sel_state_);
    model.Bind("sel_ok", &sel_ok_);
    model.Bind("show_recheck", &show_recheck_);
    model.Bind("items", &items_);
    model.Bind("result_show", &result_show_);
    model.Bind("result_ok", &result_ok_);
    model.Bind("result_label", &result_label_);
    model.Bind("result_title", &result_title_);
    model.Bind("result_lines", &result_lines_);
    model.Bind("mode", &mode_);
    model.Bind("plan_text", &plan_text_);
    model.Bind("plan_ok", &plan_ok_);
    model.Bind("new_name", &new_name_);
    model.Bind("name_error", &name_error_);
    model.Bind("code", &code_);
    model.Bind("import_show", &import_show_);
    model.Bind("import_ok", &import_ok_);
    model.Bind("import_label", &import_label_);
    model.Bind("import_title", &import_title_);
    model.Bind("import_lines", &import_lines_);

    const auto first_arg = [](const Rml::VariantList &args) {
      return args.empty() ? Rml::String() : args[0].Get<Rml::String>();
    };
    model.BindEventCallback(
        "act", [this, first_arg](Rml::DataModelHandle, Rml::Event &,
                                 const Rml::VariantList &args) {
          act(first_arg(args));
        });
    // Clicking (Enter, A) picks a pack and moves on to its actions; moving
    // focus over the list shows each pack without leaving the list.
    model.BindEventCallback(
        "select", [this, first_arg](Rml::DataModelHandle, Rml::Event &,
                                    const Rml::VariantList &args) {
          select(first_arg(args));
          focus_later("pack-apply");
        });
    model.BindEventCallback(
        "hover", [this, first_arg](Rml::DataModelHandle, Rml::Event &,
                                   const Rml::VariantList &args) {
          select(first_arg(args));
        });
    model.BindEventCallback(
        "steam", [this, first_arg](Rml::DataModelHandle, Rml::Event &,
                                   const Rml::VariantList &args) {
          open_in_steam(first_arg(args));
        });
    model.BindEventCallback(
        "mode", [this, first_arg](Rml::DataModelHandle, Rml::Event &,
                                  const Rml::VariantList &args) {
          owner_->set_mode(first_arg(args));
        });
    // Text fields: Enter does what the field's button does. The action runs
    // on the next frame: data-value's own listener for this same change
    // event runs after this one and would write the typed text back over a
    // field the action just cleared.
    model.BindEventCallback(
        "field", [this, first_arg](Rml::DataModelHandle, Rml::Event &event,
                                   const Rml::VariantList &args) {
          const std::string which = first_arg(args);
          if (!event.GetParameter<bool>("linebreak", false)) {
            return;
          }
          const Rml::String value = event.GetParameter<Rml::String>("value", "");
          if (which == "name") {
            owner_->post([this, value] {
              new_name_ = value;
              act("save");
            });
          } else if (which == "code") {
            owner_->post([this, value] {
              code_ = value;
              act("import code");
            });
          }
        });
    handle_ = model.GetModelHandle();
    return true;
  }

  void loaded(Rml::ElementDocument *document) override {
    // Placeholders are attributes, which the translation table does not see.
    if (Rml::Element *name = document->GetElementById("pack-name")) {
      name->SetAttribute("placeholder", owner_->tr("Name this pack"));
    }
    if (Rml::Element *code = document->GetElementById("share-code")) {
      code->SetAttribute("placeholder", owner_->tr("Paste a code: XRC1..."));
    }
    // Read the library now, so the list exists before the screen is shown
    // and the first focus can land on it.
    reload();
    refresh();
  }

  void shown() override {
    // Files may have been added or removed while the screen was away.
    reload();
    refresh();
    // Shown in the same frame it was loaded (the first screen), the list rows
    // do not exist yet and the app's first focus falls back to Save; move it
    // to the selected pack once they do.
    const std::string row = row_id_of(selected_id_);
    if (!row.empty() && doc && !doc->GetElementById(row)) {
      focus_later(row);
    }
  }

  void update(double) override {
    if (!focus_id_.empty() && doc) {
      if (Rml::Element *target = doc->GetElementById(focus_id_);
          target && target->IsVisible(true)) {
        target->Focus(true);
        target->ScrollIntoView(Rml::ScrollAlignment::Nearest);
        focus_id_.clear();
      } else if (--focus_tries_ <= 0) {
        focus_id_.clear();
      }
    }
    services &svc = owner_->svc();
    const std::uint64_t key[] = {svc.config_rev(), svc.catalog_rev(),
                                 svc.game_rev(), owner_->mode_rev()};
    if (std::equal(std::begin(key), std::end(key), std::begin(seen_))) {
      return;
    }
    std::copy(std::begin(key), std::end(key), std::begin(seen_));
    refresh();
  }

  Rml::Element *default_focus() override {
    if (!doc) {
      return nullptr;
    }
    const std::string wanted = row_id_of(selected_id_);
    for (const std::string &id : {wanted, std::string("pack-0"),
                                  std::string("pack-save")}) {
      if (id.empty()) {
        continue;
      }
      if (Rml::Element *element = doc->GetElementById(id);
          element && element->IsVisible(true)) {
        return element;
      }
    }
    return view::default_focus();
  }

  bool key(Rml::Input::KeyIdentifier key, int modifiers) override {
    // Delete on a pack in the list asks to delete that pack.
    if (key != Rml::Input::KI_DELETE || modifiers != 0 || !doc) {
      return false;
    }
    Rml::Element *focus = doc->GetContext()
                              ? doc->GetContext()->GetFocusElement()
                              : nullptr;
    if (!focus || !focus->IsClassSet("pack-row")) {
      return false;
    }
    const std::string id = focus->GetId();
    if (!id.starts_with("pack-")) {
      return false;
    }
    const std::size_t index = std::strtoul(id.c_str() + 5, nullptr, 10);
    if (index >= rows_.size()) {
      return false;
    }
    select(rows_[index].id);
    act("delete");
    return true;
  }

private:
  // ------------------------------------------------------------- the data
  fs::path folder() const { return owner_->svc().paths().config / "packs"; }

  void reload() {
    library_ = core::read_pack_library(folder());
    for (const auto &d : library_.diags.items()) {
      log_warn("launcher.packs.file", d.message);
    }
    if (!selected_id_.empty() && !library_.find(selected_id_)) {
      selected_id_.clear();
    }
    if (selected_id_.empty() && !library_.packs.empty()) {
      selected_id_ = library_.packs.front().value.id;
    }
  }

  const core::library_entry *selected() const {
    return selected_id_.empty() ? nullptr : library_.find(selected_id_);
  }

  std::string row_id_of(const std::string &pack_id) const {
    for (std::size_t i = 0; i < library_.packs.size(); ++i) {
      if (library_.packs[i].value.id == pack_id) {
        return "pack-" + std::to_string(i);
      }
    }
    return {};
  }

  bool catalog_ready() const {
    const auto &catalog = owner_->svc().catalog();
    return catalog.scanned || !catalog.scanning;
  }

  // Legacy Workshop items are versioned by their update time
  // ("0.0.0+ws.1700000000"); a date reads better than the number.
  std::string shown_version(const std::string &text) const {
    constexpr std::string_view legacy = "0.0.0+ws.";
    if (!text.starts_with(legacy)) {
      return text;
    }
    long long seconds = 0;
    const char *begin = text.data() + legacy.size();
    const char *end = text.data() + text.size();
    const auto parsed = std::from_chars(begin, end, seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != end || seconds < 1000000000 ||
        seconds > 4000000000) {
      return text;
    }
    const std::time_t t = static_cast<std::time_t>(seconds);
    std::tm utc{};
    gmtime_s(&utc, &t);
    char date[32];
    std::strftime(date, sizeof(date), "%Y-%m-%d", &utc);
    return owner_->tr("Workshop") + " " + date;
  }

  std::string state_text(const core::pack_item_check &item) const {
    switch (item.state) {
    case core::pack_item_state::installed: return owner_->tr("Installed");
    case core::pack_item_state::missing: return owner_->tr("Not installed");
    case core::pack_item_state::other_version: return owner_->tr("Other version");
    case core::pack_item_state::changed: return owner_->tr("Files changed");
    }
    return {};
  }

  std::string problems_text(const core::pack_check &check) const {
    std::string out;
    const auto add = [&](std::size_t n, const char *what) {
      if (n == 0) {
        return;
      }
      if (!out.empty()) {
        out += ", ";
      }
      out += std::to_string(n) + " " + owner_->tr(what);
    };
    add(check.missing, "missing");
    add(check.other_version, "other version");
    add(check.changed, "changed");
    return out;
  }

  void refresh() {
    services &svc = owner_->svc();
    const auto &catalog = svc.catalog();
    const auto &config = svc.config();
    const bool ready = catalog_ready();

    // ------------------------------------------------------------ the list
    Rml::Vector<pack_row> rows;
    for (const auto &entry : library_.packs) {
      const core::pack &p = entry.value;
      const core::pack_check check = core::check_pack(p, catalog.items);
      pack_row row;
      row.id = p.id;
      row.name = p.name;
      row.selected = p.id == selected_id_;
      row.meta = owner_->tr(mode_name(p.mode)) + " \xC2\xB7 " +
                 std::to_string(p.mods.size()) + " " +
                 owner_->tr(p.mods.size() == 1 ? "mod" : "mods");
      if (p.map) {
        row.meta += " \xC2\xB7 " + owner_->tr("map");
      }
      if (const auto it = config.profiles.find(p.mode);
          it != config.profiles.end() && it->second.pack_id == p.id) {
        row.meta += " \xC2\xB7 " + owner_->tr("in use");
      }
      if (!ready) {
        row.state = owner_->tr("Checking...");
      } else if (check.ready()) {
        row.state = owner_->tr("Ready");
      } else {
        row.state = problems_text(check);
        row.bad = true;
      }
      rows.push_back(std::move(row));
    }
    set_if_changed("packs", rows_, rows);
    set_if_changed("count_text", count_text_,
                   Rml::String(std::to_string(library_.packs.size()) + " " +
                               owner_->tr(library_.packs.size() == 1 ? "pack"
                                                                     : "packs")));
    set_if_changed("empty_text", empty_text_,
                   Rml::String(owner_->tr(
                       "No packs yet. Save the current plan as a pack, or "
                       "import one from a share code or a file.")));
    std::string note;
    if (const std::size_t skipped =
            library_.diags.count(core::severity::warning);
        skipped > 0) {
      note = std::to_string(skipped) + " " +
             owner_->tr(skipped == 1 ? "file in the packs folder was skipped"
                                     : "files in the packs folder were skipped") +
             ": " + library_.diags.items().front().message;
    }
    set_if_changed("library_note", library_note_, Rml::String(note));

    // ------------------------------------------------------- the selection
    const core::library_entry *entry = selected();
    set_if_changed("has_sel", has_sel_, entry != nullptr);
    Rml::Vector<item_row> items;
    if (entry) {
      const core::pack &p = entry->value;
      const core::pack_check check = core::check_pack(p, catalog.items);
      std::string meta = owner_->tr(mode_name(p.mode)) + " \xC2\xB7 " +
                         std::to_string(p.mods.size()) + " " +
                         owner_->tr(p.mods.size() == 1 ? "mod" : "mods");
      if (p.map) {
        meta += " \xC2\xB7 1 " + owner_->tr("map");
      }
      meta += " \xC2\xB7 " + owner_->tr("id") + " " + short_id(p.id);
      if (!p.game_build.empty()) {
        meta += " \xC2\xB7 " + owner_->tr("build") + " " + p.game_build;
      }
      if (!p.requires_xerces.empty()) {
        meta += " \xC2\xB7 " + owner_->tr("needs Xerces") + " " +
                p.requires_xerces;
      }
      set_if_changed("sel_name", sel_name_, Rml::String(p.name));
      set_if_changed("sel_meta", sel_meta_, Rml::String(meta));
      std::string state;
      if (!ready) {
        state = owner_->tr("Checking what is installed...");
      } else if (check.ready()) {
        state = owner_->tr("Everything is installed as pinned");
      } else {
        state = problems_text(check);
      }
      set_if_changed("sel_state", sel_state_, Rml::String(state));
      set_if_changed("sel_ok", sel_ok_, !ready || check.ready());
      std::size_t n = 0;
      for (const auto &item : check.items) {
        item_row row;
        row.n = item.is_map ? owner_->tr("MAP") : two_digits(++n);
        row.name = item.name;
        row.sub = item.name != item.id ? item.id : "";
        if (item.state == core::pack_item_state::other_version) {
          row.sub = (row.sub.empty() ? "" : row.sub + " \xC2\xB7 ") +
                    owner_->tr("installed") + " " +
                    shown_version(item.installed);
        }
        row.ver = item.is_map ? owner_->tr("by its files")
                              : shown_version(item.pinned);
        row.state = ready ? state_text(item) : owner_->tr("Checking...");
        row.bad = ready && item.state != core::pack_item_state::installed;
        // Steam is where a missing or out-of-date Workshop item is fixed.
        if (row.bad && item.workshop_id) {
          row.ws = std::to_string(*item.workshop_id);
        }
        items.push_back(std::move(row));
      }
    }
    set_if_changed("items", items_, items);
    // The applied box stays only while the pack is still what its mode uses
    // (the Mods screen clears that when the load order is edited by hand).
    if (!result_pack_.empty()) {
      const core::library_entry *applied = library_.find(result_pack_);
      const auto profile =
          applied ? config.profiles.find(applied->value.mode)
                  : config.profiles.end();
      if (profile == config.profiles.end() ||
          profile->second.pack_id != result_pack_) {
        result_pack_.clear();
      }
    }
    const bool for_this = entry && result_pack_ == entry->value.id;
    if (for_this) {
      fill_result(entry->value);
    }
    set_if_changed("result_show", result_show_, for_this);
    set_if_changed("show_recheck", show_recheck_,
                   entry != nullptr && !sel_ok_ && !for_this);

    // ------------------------------------------------- saving the plan
    set_if_changed("mode", mode_, Rml::String(owner_->mode()));
    std::string plan_text;
    bool plan_ok = false;
    const char *mode = mode_name(owner_->mode());
    if (!ready) {
      plan_text = owner_->tr("Reading your mods...");
    } else {
      const core::plan plan = svc.resolve(owner_->mode());
      if (plan.stack.empty() && plan.ok()) {
        plan_text = owner_->tr("No mods are enabled for") + " " +
                    owner_->tr(mode) + ". " +
                    owner_->tr("Enable some in Mods first.");
      } else if (!plan.ok()) {
        const std::size_t errors = plan.diags.count(core::severity::error);
        plan_text = owner_->tr("The") + " " + owner_->tr(mode) + " " +
                    owner_->tr("plan has") + " " + std::to_string(errors) +
                    " " + owner_->tr(errors == 1 ? "problem" : "problems") +
                    ". " + owner_->tr("Fix them in Mods first.");
      } else {
        plan_ok = true;
        plan_text = std::to_string(plan.stack.size()) + " " +
                    owner_->tr(plan.stack.size() == 1 ? "mod" : "mods") +
                    (plan.map ? " + " + owner_->tr("map") : std::string()) +
                    " " + owner_->tr("in the") + " " + owner_->tr(mode) + " " +
                    owner_->tr("plan, pinned at their installed versions.");
      }
    }
    set_if_changed("plan_text", plan_text_, Rml::String(plan_text));
    set_if_changed("plan_ok", plan_ok_, plan_ok);
  }

  template <typename T>
  void set_if_changed(const char *name, T &variable, const T &value) {
    // Rebuilding a list under the focused row would drop the focus, so only
    // what changed is marked dirty.
    if (!(variable == value)) {
      variable = value;
      handle_.DirtyVariable(name);
    }
  }

  void select(const std::string &id) {
    if (id.empty() || !library_.find(id) || id == selected_id_) {
      return;
    }
    selected_id_ = id;
    refresh();
  }

  void focus_later(const std::string &element_id) {
    // Elements made by data-for and data-if appear after the model updates,
    // so focus waits for the next frames.
    focus_id_ = element_id;
    focus_tries_ = 30;
  }

  void notify(const std::string &text, bool error = false) {
    owner_->notify(text, error);
  }

  void backup(const std::string &why) {
    const auto &paths = owner_->svc().paths();
    core::diagnostics diags;
    const auto made = core::create_backup(paths.home / "backups",
                                          {{"config", paths.config}},
                                          backup_stamp(), {}, diags);
    if (made) {
      log_info("launcher.packs.backup", why, "");
    } else {
      // A failed backup is logged, not fatal: the change is still the one
      // the player asked for.
      log_warn("launcher.packs.backup_failed",
               diags.items().empty() ? why : diags.items().front().message);
    }
  }

  // ------------------------------------------------------------ actions
  void act(const std::string &action) {
    log_info("launcher.packs.action", action);
    if (action == "save") {
      save();
    } else if (action == "apply") {
      apply();
    } else if (action == "export") {
      export_file();
    } else if (action == "copy code") {
      copy_code();
    } else if (action == "delete") {
      remove();
    } else if (action == "import code") {
      import_code();
    } else if (action == "import file") {
      import_file();
    } else if (action == "recheck") {
      owner_->svc().start_scan();
      notify(owner_->tr("Reading your mods again..."));
    } else if (action == "home") {
      owner_->show("home");
    } else if (action == "mods") {
      owner_->show("mods");
    }
  }

  void save() {
    services &svc = owner_->svc();
    const std::string name = core::trim_pack_name(new_name_);
    if (const std::string problem = core::pack_name_problem(name);
        !problem.empty()) {
      set_if_changed("name_error", name_error_, Rml::String(owner_->tr(problem)));
      focus_later("pack-name");
      return;
    }
    if (!catalog_ready()) {
      notify(owner_->tr("Xerces is still reading your mods. Try again in a "
                        "moment."),
             true);
      return;
    }
    const std::string mode = owner_->mode();
    const core::plan plan = svc.resolve(mode);
    if (!plan.ok()) {
      set_if_changed("name_error", name_error_,
                     Rml::String(owner_->tr("The plan has problems. Fix them "
                                            "in Mods, then save it as a pack.")));
      return;
    }
    if (plan.stack.empty()) {
      set_if_changed("name_error", name_error_,
                     Rml::String(owner_->tr("No mods are enabled for this "
                                            "mode, so there is nothing to "
                                            "save.")));
      return;
    }
    // Workshop ids come from each installed manifest, so the pack can send
    // people to the right Steam page.
    std::map<std::string, std::uint64_t> workshop_ids;
    for (const auto &entry : plan.stack) {
      if (const auto *content = svc.catalog().find(entry.id);
          content && content->info.workshop_id) {
        workshop_ids[entry.id] = *content->info.workshop_id;
      }
    }
    core::pack p =
        core::pack_from_plan(plan, name, core::new_pack_id(), workshop_ids);
    // The build it was made on, as the CLI's export records it; information
    // for whoever imports it, not a requirement.
    if (svc.game().steam_build) {
      p.game_build = std::to_string(*svc.game().steam_build);
    }
    if (const core::library_entry *same = core::find_by_name(library_, name)) {
      const std::string existing_id = same->value.id;
      owner_->dialog(
          "A pack with this name exists",
          quoted(same->value.name) + " " +
              owner_->tr("is already in your packs. Replace it with the "
                         "current plan, or keep both?"),
          {"Replace it", "Keep both", "Cancel"},
          [this, p, existing_id](int choice) mutable {
            if (choice == 0) {
              backup("replace pack " + existing_id);
              p.id = existing_id;
              store(p, "Replaced");
            } else if (choice == 1) {
              store(p, "Saved");
            }
          });
      return;
    }
    store(p, "Saved");
  }

  void store(const core::pack &p, const char *verb) {
    core::diagnostics diags;
    if (!core::write_pack(folder(), p, diags)) {
      const std::string why =
          diags.items().empty() ? owner_->tr("The pack could not be saved.")
                                : diags.items().front().message;
      set_if_changed("name_error", name_error_, Rml::String(why));
      notify(owner_->tr("The pack was not saved."), true);
      log_warn("launcher.packs.save_failed", why);
      return;
    }
    log_info("launcher.packs.saved", p.id,
             "{\"mods\":" + std::to_string(p.mods.size()) + "}");
    set_if_changed("new_name", new_name_, Rml::String());
    set_if_changed("name_error", name_error_, Rml::String());
    reload();
    selected_id_ = p.id;
    refresh();
    notify(owner_->tr(verb) + " " + quoted(p.name) + ".");
    focus_later(row_id_of(p.id));
  }

  void apply() {
    const core::library_entry *entry = selected();
    if (!entry) {
      return;
    }
    if (!catalog_ready()) {
      notify(owner_->tr("Xerces is still reading your mods. Try again in a "
                        "moment."),
             true);
      return;
    }
    const core::pack p = entry->value;
    const auto &config = owner_->svc().config();
    const auto it = config.profiles.find(p.mode);
    const bool replaces = it != config.profiles.end() &&
                          !it->second.mods.empty() &&
                          !(it->second.pack_id == p.id &&
                            it->second.mods == p.order);
    if (!replaces) {
      apply_now(p.id);
      return;
    }
    const std::size_t had = it->second.mods.size();
    const std::string from =
        it->second.pack_name.empty()
            ? std::string()
            : " (" + owner_->tr("from") + " " + quoted(it->second.pack_name) +
                  ")";
    owner_->dialog(
        "Replace this profile?",
        owner_->tr(mode_name(p.mode)) + " " + owner_->tr("has") + " " +
            std::to_string(had) + " " +
            owner_->tr(had == 1 ? "mod" : "mods") + " " +
            owner_->tr("enabled") + from + ". " + owner_->tr("Applying") + " " +
            quoted(p.name) + " " + owner_->tr("replaces them with its") + " " +
            std::to_string(p.order.size()) + " " +
            owner_->tr("pinned") + " " +
            owner_->tr(p.order.size() == 1 ? "mod" : "mods") + ". " +
            owner_->tr("Your config is backed up first."),
        {"Apply pack", "Cancel"}, [this, id = p.id](int choice) {
          if (choice == 0) {
            apply_now(id);
          } else {
            focus_later("pack-apply");
          }
        });
  }

  void apply_now(const std::string &id) {
    const core::library_entry *entry = library_.find(id);
    if (!entry) {
      return;
    }
    const core::pack p = entry->value;
    services &svc = owner_->svc();
    backup("apply pack " + p.id);

    // The profile becomes the pack: its order, its exact versions (8.3).
    core::mode_profile &profile = core::profile_for(svc.config(), p.mode);
    profile.mods = p.order;
    profile.pins.clear();
    for (const auto &mod : p.mods) {
      profile.pins[mod.id] = mod.ver.to_string();
    }
    profile.map = p.map ? std::optional<std::string>(p.map->id) : std::nullopt;
    profile.pack_name = p.name;
    profile.pack_id = p.id;
    result_saved_ = svc.save_config();
    owner_->set_mode(p.mode);
    result_pack_ = p.id;
    selected_id_ = p.id;
    refresh(); // fills the result box through fill_result

    log_info("launcher.packs.applied", p.id,
             "{\"ok\":" + std::string(result_ok_ ? "true" : "false") +
                 ",\"problems\":" + std::to_string(result_problems_) + "}");
    notify(result_ok_
               ? owner_->tr("Applied") + " " + quoted(p.name) + "."
               : owner_->tr("Applied") + " " + quoted(p.name) + ": " +
                     std::to_string(result_problems_) + " " +
                     owner_->tr(result_problems_ == 1 ? "problem to fix"
                                                      : "problems to fix") +
                     ".",
           !result_ok_);
    if (!result_ok_) {
      // The first Steam button, when there is something to get from Steam.
      for (std::size_t i = 0; i < items_.size(); ++i) {
        if (!items_[i].ws.empty()) {
          focus_later("steam-" + std::to_string(i));
          return;
        }
      }
      focus_later("result-recheck");
    } else {
      focus_later("result-home");
    }
  }

  // The applied pack against what is installed now (8.3, verify): the
  // plan's errors and verify_pack_content's mismatches. Run on every refresh
  // while the box shows, so "Check again" and Steam downloads update it.
  void fill_result(const core::pack &p) {
    services &svc = owner_->svc();
    const core::plan plan = svc.resolve(p.mode);
    const core::diagnostics verify = core::verify_pack_content(p, plan);
    const core::pack_check check = core::check_pack(p, svc.catalog().items);
    std::vector<std::string> lines;
    const auto add = [&](const std::string &message) {
      if (std::ranges::find(lines, message) == lines.end()) {
        lines.push_back(message);
      }
    };
    for (const auto &d : plan.diags.items()) {
      if (d.level == core::severity::error) {
        add(d.message);
      }
    }
    for (const auto &d : verify.items()) {
      add(d.message);
    }
    if (!result_saved_) {
      add(owner_->tr("The config could not be written, so this change is "
                     "lost when Xerces closes."));
    }
    const bool ok = result_saved_ && plan.ok() && !verify.has_errors() &&
                    check.ready();
    result_problems_ = lines.size();
    constexpr std::size_t shown = 6;
    Rml::Vector<Rml::String> result_lines(
        lines.begin(), lines.begin() + std::min(lines.size(), shown));
    if (lines.size() > shown) {
      result_lines.push_back("+" + std::to_string(lines.size() - shown) + " " +
                             owner_->tr("more"));
    }
    const std::string mode = owner_->tr(mode_name(p.mode));
    set_if_changed("result_ok", result_ok_, ok);
    set_if_changed("result_label", result_label_,
                   Rml::String(owner_->tr(ok ? "Applied" : "Applied, not ready")));
    set_if_changed(
        "result_title", result_title_,
        Rml::String(ok ? mode + " " +
                             owner_->tr("now uses this pack. Launch it from Home.")
                       : mode + " " +
                             owner_->tr("now uses this pack, but it will not "
                                        "launch modded until these are fixed:")));
    set_if_changed("result_lines", result_lines_, result_lines);
  }

  void export_file() {
    const core::library_entry *entry = selected();
    if (!entry) {
      return;
    }
    const core::pack p = entry->value;
    const std::wstring suggested =
        from_utf8(core::pack_file_stem(p.name) + ".xpack.json").wstring();
    const auto picked = owner_->pick_save_file(
        "Export pack", L"Xerces pack (*.xpack.json)|*.xpack.json", suggested);
    if (!picked) {
      return; // cancelled
    }
    fs::path file = *picked;
    // The picker adds no extension of its own; a bare name gets the pack one.
    if (!file.has_extension()) {
      file += ".xpack.json";
      std::error_code ec;
      if (fs::exists(file, ec)) {
        owner_->dialog("Replace the file?",
                       quoted(utf8(file.filename())) + " " +
                           owner_->tr("already exists. Replace it?"),
                       {"Replace", "Cancel"},
                       [this, file, p](int choice) {
                         if (choice == 0) {
                           write_export(file, p);
                         }
                       });
        return;
      }
    }
    write_export(file, p);
  }

  void write_export(const fs::path &file, const core::pack &p) {
    if (!write_file(file, core::pack_to_json(p))) {
      notify(owner_->tr("The pack could not be written to") + " " +
                 utf8(file.filename()) + ".",
             true);
      log_warn("launcher.packs.export_failed", utf8(file));
      return;
    }
    log_info("launcher.packs.exported", p.id);
    notify(owner_->tr("Exported to") + " " + utf8(file.filename()) + ".");
  }

  void copy_code() {
    const core::library_entry *entry = selected();
    if (!entry) {
      return;
    }
    const std::string code = core::encode_share_code(entry->value);
    if (code.empty()) {
      notify(owner_->tr("The share code could not be made."), true);
      return;
    }
    owner_->copy_text(code);
    log_info("launcher.packs.code_copied", entry->value.id,
             "{\"chars\":" + std::to_string(code.size()) + "}");
    notify(owner_->tr("Share code copied") + " (" +
           std::to_string(code.size()) + " " + owner_->tr("characters") +
           "). " + owner_->tr("Anyone with Xerces can import it."));
  }

  void remove() {
    const core::library_entry *entry = selected();
    if (!entry) {
      return;
    }
    const std::string id = entry->value.id;
    const std::string name = entry->value.name;
    owner_->dialog(
        "Delete this pack?",
        quoted(name) + " " +
            owner_->tr("is removed from your packs. Your mods stay installed, "
                       "and a profile it was applied to keeps its mods."),
        {"Delete", "Cancel"}, [this, id, name](int choice) {
          if (choice != 0) {
            focus_later("pack-delete");
            return;
          }
          // The neighbour takes the selection, so focus stays in the list.
          std::size_t index = 0;
          for (std::size_t i = 0; i < library_.packs.size(); ++i) {
            if (library_.packs[i].value.id == id) {
              index = i;
            }
          }
          backup("delete pack " + id);
          if (!core::remove_pack(folder(), id)) {
            notify(owner_->tr("The pack file could not be deleted."), true);
            return;
          }
          log_info("launcher.packs.deleted", id);
          if (result_pack_ == id) {
            result_pack_.clear();
          }
          selected_id_.clear();
          reload();
          if (!library_.packs.empty()) {
            selected_id_ =
                library_.packs[std::min(index, library_.packs.size() - 1)]
                    .value.id;
          }
          refresh();
          notify(owner_->tr("Deleted") + " " + quoted(name) + ".");
          focus_later(selected_id_.empty() ? "pack-save"
                                           : row_id_of(selected_id_));
        });
  }

  void import_code() {
    const std::string text = core::trim_pack_name(code_);
    if (text.empty()) {
      show_import(false, "Nothing to import",
                  owner_->tr("Paste a share code into the field first."), {});
      focus_later("share-code");
      return;
    }
    finish_import(core::read_pack_text(text), owner_->tr("share code"), true);
  }

  void import_file() {
    const auto picked = owner_->pick_open_file(
        "Import pack",
        L"Xerces pack (*.xpack.json)|*.xpack.json|JSON files (*.json)|*.json|"
        L"All files (*.*)|*.*");
    if (!picked) {
      return; // cancelled
    }
    finish_import(core::read_pack_file(*picked),
                  owner_->tr("file") + " " + utf8(picked->filename()), false);
  }

  void show_import(bool ok, const char *label, const std::string &title,
                   const std::vector<std::string> &lines) {
    set_if_changed("import_show", import_show_, true);
    set_if_changed("import_ok", import_ok_, ok);
    set_if_changed("import_label", import_label_, Rml::String(owner_->tr(label)));
    set_if_changed("import_title", import_title_, Rml::String(title));
    Rml::Vector<Rml::String> shown(lines.begin(),
                                   lines.begin() + std::min<std::size_t>(lines.size(), 6));
    if (lines.size() > 6) {
      shown.push_back("+" + std::to_string(lines.size() - 6) + " " +
                      owner_->tr("more"));
    }
    set_if_changed("import_lines", import_lines_, shown);
  }

  void finish_import(core::pack_result result, const std::string &source,
                     bool from_code) {
    std::vector<std::string> errors, notes;
    for (const auto &d : result.diags.items()) {
      (d.level == core::severity::error ? errors : notes).push_back(d.message);
    }
    if (!result.ok()) {
      show_import(false, "Not imported",
                  owner_->tr("This") + " " + source + " " +
                      owner_->tr("is not a pack Xerces can read:"),
                  errors);
      notify(owner_->tr("Import failed: the reasons are under Import."), true);
      log_warn("launcher.packs.import_failed",
               errors.empty() ? "" : errors.front());
      return;
    }
    const core::import_placement placed =
        core::place_import(library_, std::move(result.value));
    const core::pack &p = placed.value;
    if (placed.kind == core::import_kind::duplicate) {
      show_import(true, "Already here",
                  quoted(p.name) + " " +
                      owner_->tr("is already in your packs, unchanged."),
                  notes);
    } else {
      core::diagnostics diags;
      if (!core::write_pack(folder(), p, diags)) {
        std::vector<std::string> why;
        for (const auto &d : diags.items()) {
          why.push_back(d.message);
        }
        show_import(false, "Not imported",
                    owner_->tr("The pack could not be saved:"), why);
        notify(owner_->tr("Import failed."), true);
        return;
      }
      if (placed.kind == core::import_kind::renumbered) {
        notes.insert(notes.begin(),
                     owner_->tr("It was saved as a new pack, because a "
                                "different pack of yours has its id."));
      }
      show_import(true, "Imported",
                  quoted(p.name) + " " +
                      owner_->tr("was added to your packs. Check its mods, "
                                 "then apply it."),
                  notes);
      log_info("launcher.packs.imported", p.id,
               std::string("{\"from\":\"") + (from_code ? "code" : "file") +
                   "\",\"renumbered\":" +
                   (placed.kind == core::import_kind::renumbered ? "true"
                                                                 : "false") +
                   "}");
    }
    if (from_code) {
      set_if_changed("code", code_, Rml::String());
    }
    const std::string id = p.id;
    const std::string name = p.name;
    reload();
    selected_id_ = id;
    refresh();
    notify(placed.kind == core::import_kind::duplicate
               ? owner_->tr("Already in your packs:") + " " + quoted(name) + "."
               : owner_->tr("Imported") + " " + quoted(name) + ".");
    focus_later("pack-apply");
  }

  void open_in_steam(const std::string &workshop_id) {
    if (workshop_id.empty() ||
        !std::ranges::all_of(workshop_id, [](char c) {
          return c >= '0' && c <= '9';
        })) {
      return;
    }
    if (owner_->open_url("steam://url/CommunityFilePage/" + workshop_id)) {
      notify(owner_->tr("Opened the Workshop page in Steam. Subscribe or "
                        "update there, then check again."));
    } else {
      notify(owner_->tr("Steam could not be opened."), true);
    }
  }

  // ----------------------------------------------------------- the state
  app *owner_ = nullptr;
  Rml::DataModelHandle handle_;
  std::uint64_t seen_[4] = {};
  core::pack_library library_;
  std::string selected_id_;
  std::string result_pack_; // the pack the result box is about
  bool result_saved_ = true;  // its apply wrote the config
  std::size_t result_problems_ = 0;
  std::string focus_id_;
  int focus_tries_ = 0;

  Rml::Vector<pack_row> rows_;
  Rml::String count_text_, empty_text_, library_note_;
  bool has_sel_ = false;
  Rml::String sel_name_, sel_meta_, sel_state_;
  bool sel_ok_ = true, show_recheck_ = false;
  Rml::Vector<item_row> items_;
  bool result_show_ = false, result_ok_ = false;
  Rml::String result_label_, result_title_;
  Rml::Vector<Rml::String> result_lines_;
  Rml::String mode_ = "zm", plan_text_;
  bool plan_ok_ = false;
  Rml::String new_name_, name_error_, code_;
  bool import_show_ = false, import_ok_ = false;
  Rml::String import_label_, import_title_;
  Rml::Vector<Rml::String> import_lines_;
};
} // namespace

XERCES_VIEW(packs_view, "packs", 30, true);
} // namespace xerces_launcher
