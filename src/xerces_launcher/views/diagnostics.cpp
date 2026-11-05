// Diagnostics (docs/DESIGN.md 7.3, 4.2, 5.10, 15.1): what an expert or a bug
// report needs. The build report (game folder, BlackOps3.exe facts, the Steam
// build, the pre-flight verdict and the compat/builds.json entry it matched,
// the client), the signature report the client wrote at its last start, the
// mods found with the scan's diagnostics, the last launch and safe mode, the
// logs and crash bundles. Copy report puts all of it on the clipboard as plain
// text with the profile folder hidden; Verify with Steam hands the file check
// to Steam (Xerces never changes game files); Refresh detects the game again.

#include <RmlUi/Core.h>

#include <RmlUi_Include_Windows.h>
// wingdi.h's GetObject macro would rename rapidjson's GetObject().
#undef GetObject

#include <algorithm>
#include <cstdio>
#include <map>

#include <xerces_core/compat.hpp>
#include <xerces_core/detect.hpp>
#include <xerces_core/diagnostics_report.hpp>
#include <xerces_core/journal.hpp>
#include <xerces_core/manifest.hpp>
#include <xerces_core/planner.hpp>

#include "../app.hpp"
#include "../log.hpp"

namespace xerces_launcher {
namespace {
// Rows of the data model. Types local to this file, so registering them never
// collides with another screen's types in the context's shared type register.
struct kv_row {
  Rml::String k, v;
  bool mono = false;
  bool warn = false;
};
struct text_row {
  Rml::String text;
};
struct diag_row {
  Rml::String level, code, message;
  bool error = false;
  bool warning = false;
};
struct file_row {
  Rml::String name, size, date;
};

// The scan can report hundreds of findings on a big Workshop folder; the
// screen shows the worst ones and the copied report has all of them.
constexpr std::size_t max_diag_rows = 40;
constexpr std::size_t max_crash_rows = 12;

std::string utf8(const fs::path &path) {
  const auto text = path.u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

// log_path() is '/'-separated; the screen and the report show Windows paths.
std::string native(const std::string &generic) {
  if (generic.empty()) {
    return generic;
  }
  fs::path path(std::u8string(reinterpret_cast<const char8_t *>(generic.data()),
                              generic.size()));
  return utf8(path.make_preferred());
}

std::string two_digits(std::size_t value) {
  char text[16];
  std::snprintf(text, sizeof(text), "%02zu", value);
  return text;
}

std::string seconds_text(double seconds) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f s", seconds);
  return text;
}

// "10.0.26300.1234": what Windows really is (GetVersionEx lies to programs
// without a manifest entry), plus the update revision from the registry.
std::string windows_version() {
  using get_version = LONG(WINAPI *)(OSVERSIONINFOW *);
  const auto get = reinterpret_cast<get_version>(reinterpret_cast<void *>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
  OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (!get || get(&info) != 0) {
    return "unknown";
  }
  std::string out = std::to_string(info.dwMajorVersion) + "." +
                    std::to_string(info.dwMinorVersion) + "." +
                    std::to_string(info.dwBuildNumber);
  DWORD revision = 0;
  DWORD size = sizeof(revision);
  if (RegGetValueW(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                   RRF_RT_REG_DWORD, nullptr, &revision,
                   &size) == ERROR_SUCCESS) {
    out += "." + std::to_string(revision);
  }
  return out;
}

const char *verdict_label(xerces_core::preflight_verdict verdict) {
  switch (verdict) {
  case xerces_core::preflight_verdict::supported:
    return "Supported";
  case xerces_core::preflight_verdict::upstream_only:
    return "Upstream only";
  case xerces_core::preflight_verdict::unsupported:
    return "Unsupported build";
  case xerces_core::preflight_verdict::newer:
    return "Newer than supported";
  case xerces_core::preflight_verdict::unknown:
    return "Unknown or modified";
  }
  return "Unknown or modified";
}

const char *phase_label(xerces_core::launch_phase phase) {
  switch (phase) {
  case xerces_core::launch_phase::launching:
    return "Starting";
  case xerces_core::launch_phase::symbols:
    return "Finding the game's functions";
  case xerces_core::launch_phase::frontend:
    return "In the menus";
  case xerces_core::launch_phase::loading_map:
    return "Loading a map";
  case xerces_core::launch_phase::in_game:
    return "In a match";
  case xerces_core::launch_phase::clean_exit:
    return "Exited cleanly";
  }
  return "Starting";
}

int severity_rank(xerces_core::severity level) {
  return level == xerces_core::severity::error     ? 0
         : level == xerces_core::severity::warning ? 1
                                                   : 2;
}

class diagnostics_view final : public view {
public:
  const char *id() const override { return "diagnostics"; }

  bool bind(Rml::Context *context, app &owner) override {
    owner_ = &owner;
    Rml::DataModelConstructor model = context->CreateDataModel("diagnostics");
    if (!model) {
      return false;
    }
    if (auto row = model.RegisterStruct<kv_row>()) {
      row.RegisterMember("k", &kv_row::k);
      row.RegisterMember("v", &kv_row::v);
      row.RegisterMember("mono", &kv_row::mono);
      row.RegisterMember("warn", &kv_row::warn);
    }
    if (auto row = model.RegisterStruct<text_row>()) {
      row.RegisterMember("text", &text_row::text);
    }
    if (auto row = model.RegisterStruct<diag_row>()) {
      row.RegisterMember("level", &diag_row::level);
      row.RegisterMember("code", &diag_row::code);
      row.RegisterMember("message", &diag_row::message);
      row.RegisterMember("error", &diag_row::error);
      row.RegisterMember("warning", &diag_row::warning);
    }
    if (auto row = model.RegisterStruct<file_row>()) {
      row.RegisterMember("name", &file_row::name);
      row.RegisterMember("size", &file_row::size);
      row.RegisterMember("date", &file_row::date);
    }
    model.RegisterArray<Rml::Vector<kv_row>>();
    model.RegisterArray<Rml::Vector<text_row>>();
    model.RegisterArray<Rml::Vector<diag_row>>();
    model.RegisterArray<Rml::Vector<file_row>>();

    model.Bind("busy", &busy_);
    model.Bind("refresh_label", &refresh_label_);
    model.Bind("build_value", &build_value_);
    model.Bind("build_detail", &build_detail_);
    model.Bind("build_ok", &build_ok_);
    model.Bind("build_count", &build_count_);
    model.Bind("game_found", &game_found_);
    model.Bind("build_rows", &build_rows_);
    model.Bind("build_note", &build_note_);
    model.Bind("tried", &tried_);
    model.Bind("sig_value", &sig_value_);
    model.Bind("sig_detail", &sig_detail_);
    model.Bind("sig_warn", &sig_warn_);
    model.Bind("sig_count", &sig_count_);
    model.Bind("sig_rows", &sig_rows_);
    model.Bind("sig_note", &sig_note_);
    model.Bind("missing", &missing_);
    model.Bind("catalog_value", &catalog_value_);
    model.Bind("catalog_detail", &catalog_detail_);
    model.Bind("catalog_count", &catalog_count_);
    model.Bind("catalog_rows", &catalog_rows_);
    model.Bind("diags", &diags_);
    model.Bind("diag_count", &diag_count_);
    model.Bind("diag_note", &diag_note_);
    model.Bind("diag_more", &diag_more_);
    model.Bind("launch_value", &launch_value_);
    model.Bind("launch_detail", &launch_detail_);
    model.Bind("launch_count", &launch_count_);
    model.Bind("launch_rows", &launch_rows_);
    model.Bind("launch_stack", &launch_stack_);
    model.Bind("launch_note", &launch_note_);
    model.Bind("safe_on", &safe_on_);
    model.Bind("safe_text", &safe_text_);
    model.Bind("log_count", &log_count_);
    model.Bind("log_rows", &log_rows_);
    model.Bind("crashes", &crashes_);
    model.Bind("crash_count", &crash_count_);
    model.Bind("crash_more", &crash_more_);
    model.BindEventCallback(
        "act", [this](Rml::DataModelHandle, Rml::Event &,
                      const Rml::VariantList &args) {
          if (!args.empty()) {
            act(args[0].Get<Rml::String>());
          }
        });
    model.BindEventCallback(
        "copy_diag", [this](Rml::DataModelHandle, Rml::Event &,
                            const Rml::VariantList &args) {
          if (!args.empty()) {
            copy_diag(args[0].Get<int>(-1));
          }
        });
    handle_ = model.GetModelHandle();
    return true;
  }

  void shown() override {
    // Files outside the services' counters: the journal, the logs and the
    // crash folder can change while another screen is up.
    refresh_launch();
    refresh_files();
    handle_.DirtyAllVariables();
  }

  void update(double now) override {
    if (nudge_ != 0) {
      Rml::Element *content = doc ? doc->GetElementById("content") : nullptr;
      if (content && owner_->current() == id() && !owner_->dialog_open() &&
          owner_->context()->GetFocusElement() == nudge_from_) {
        content->SetScrollTop(content->GetScrollTop() +
                              float(nudge_) * content->GetClientHeight() * 0.3f);
      }
      nudge_ = 0;
      nudge_from_ = nullptr;
    }
    services &svc = owner_->svc();
    const bool busy = svc.detecting() || svc.catalog().scanning;
    bool dirty = false;
    if (svc.game_rev() != seen_game_ || svc.config_rev() != seen_config_ ||
        busy != busy_) {
      seen_game_ = svc.game_rev();
      seen_config_ = svc.config_rev();
      if (busy_ && !busy && refresh_asked_) {
        refresh_asked_ = false;
        owner_->notify(owner_->tr("Refreshed: the reports show the game and "
                                  "mods as they are now."));
      }
      busy_ = busy;
      refresh_label_ = owner_->tr(busy ? "Refreshing..." : "Refresh");
      refresh_build();
      dirty = true;
    }
    if (svc.signatures_rev() != seen_signatures_) {
      seen_signatures_ = svc.signatures_rev();
      refresh_signatures();
      dirty = true;
    }
    // A scan bumps the counter for every folder it reads; a few updates a
    // second are enough for the progress line.
    if (svc.catalog_rev() != seen_catalog_ &&
        (!svc.catalog().scanning || now - catalog_at_ >= 0.25)) {
      seen_catalog_ = svc.catalog_rev();
      catalog_at_ = now;
      refresh_catalog();
      dirty = true;
    }
    if (svc.journal_rev() != seen_journal_) {
      seen_journal_ = svc.journal_rev();
      refresh_launch();
      refresh_files();
      dirty = true;
    }
    if (dirty) {
      handle_.DirtyAllVariables();
    }
  }

  bool key(Rml::Input::KeyIdentifier key, int modifiers) override {
    Rml::Element *content = doc ? doc->GetElementById("content") : nullptr;
    if (modifiers != 0 || !content) {
      return false;
    }
    // Page Up and Page Down scroll a page of reports.
    if (key == Rml::Input::KI_PRIOR || key == Rml::Input::KI_NEXT) {
      const float page = content->GetClientHeight() * 0.85f;
      content->SetScrollTop(content->GetScrollTop() +
                            (key == Rml::Input::KI_NEXT ? page : -page));
      return true;
    }
    // Most of this screen is text, not buttons. Up and Down still move focus
    // (spatial navigation runs after this); when there is no button left in
    // that direction the focus stays put, and update() scrolls instead, so a
    // controller can read to the end of the page.
    if (key == Rml::Input::KI_DOWN || key == Rml::Input::KI_UP) {
      Rml::Element *focus = owner_->context()->GetFocusElement();
      for (Rml::Element *e = focus; e; e = e->GetParentNode()) {
        if (e == content) {
          nudge_from_ = focus;
          nudge_ = key == Rml::Input::KI_DOWN ? 1 : -1;
          break;
        }
      }
    }
    return false;
  }

private:
  // ----------------------------------------------------------------- build
  void refresh_build() {
    services &svc = owner_->svc();
    const game_state &game = svc.game();
    const launcher_paths &paths = svc.paths();
    build_rows_.clear();
    tried_.clear();
    build_note_.clear();
    game_found_ = game.dir.has_value();
    build_ok_ = game.can_launch();

    build_count_ = !game.searched || svc.detecting() ? owner_->tr("looking")
                   : !game.dir     ? owner_->tr("not found")
                   : game.preflight ? xerces_core::to_string(game.preflight->verdict)
                                    : owner_->tr("not checked");
    if (!game.searched || svc.detecting()) {
      build_value_ = owner_->tr("Looking...");
      build_detail_ = owner_->tr("Reading BlackOps3.exe");
      build_ok_ = true; // not a problem yet
      build_note_ =
          owner_->tr("Looking for Black Ops III and reading BlackOps3.exe...");
    } else if (!game.dir) {
      build_value_ = owner_->tr("Not found");
      build_detail_ = owner_->tr("Set the game folder in Settings");
    } else if (game.preflight) {
      build_value_ = owner_->tr(verdict_label(game.preflight->verdict));
      build_detail_ =
          game.steam_build
              ? owner_->tr("Steam build") + " " + std::to_string(*game.steam_build)
              : owner_->tr("Steam build unknown");
    } else {
      build_value_ = owner_->tr("Not checked");
      build_detail_ = game.error;
    }

    const auto row = [this](const std::string &name, std::string value,
                            bool mono = false, bool warn = false) {
      build_rows_.push_back({owner_->tr(name), std::move(value), mono, warn});
    };
    if (game.searched) {
      row("Game folder",
          game.dir ? utf8(*game.dir) : owner_->tr("Not found"), game.dir.has_value(),
          !game.dir);
      if (game.dir) {
        row("Steam build",
            game.steam_build ? std::to_string(*game.steam_build)
                             : owner_->tr("Unknown (no appmanifest)"),
            true);
      }
      if (game.exe) {
        const xerces_core::exe_facts &exe = *game.exe;
        row("Exe size",
            xerces_core::format_count(exe.size) + " " + owner_->tr("bytes") +
                " (" + xerces_core::format_bytes(exe.size) + ")",
            true);
        row("Checksum", xerces_core::format_hex32(exe.checksum), true);
        row("Link time",
            xerces_core::format_hex32(exe.link_time) + "  " +
                xerces_core::format_unix_time(exe.link_time),
            true);
        row("SHA-256", xerces_core::short_hash(exe.sha256, 16) + "...", true);
      }
      if (game.preflight) {
        const auto &check = *game.preflight;
        row("Pre-flight", xerces_core::to_string(check.verdict), true,
            !check.can_launch());
        if (!check.message.empty()) {
          row("Message", check.message, false, !check.can_launch());
        }
        row("Matched build",
            check.build ? check.build->name + " (" +
                              xerces_core::to_string(check.build->status) + ")"
                        : owner_->tr("No compat/builds.json entry matched"),
            false, !check.build);
      }
      if (!game.error.empty()) {
        row("Problem", game.error, false, true);
      }
      if (!game.dir) {
        for (const std::string &place : game.tried) {
          tried_.push_back({place});
        }
      }
    }
    std::error_code ec;
    const bool client = fs::exists(paths.client, ec);
    row("Client", utf8(paths.client), true);
    row("Client file",
        client ? owner_->tr("Present")
               : owner_->tr("Missing: launches need it next to Xerces.exe"),
        false, !client);
    row("Xerces", services::version() +
                      (svc.dry_launch() ? "  (" + owner_->tr("dry launch") + ")"
                                        : std::string()),
        true);
    row("Home folder", utf8(paths.home), true);
    if (!svc.config_error().empty()) {
      row("Settings file", svc.config_error(), false, true);
    }
  }

  // ------------------------------------------------------------ signatures
  void refresh_signatures() {
    const signature_state &sigs = owner_->svc().signatures();
    sig_rows_.clear();
    missing_.clear();
    sig_note_.clear();
    sig_warn_ = sigs.known && !sigs.missing.empty();
    if (!sigs.known) {
      sig_value_ = owner_->tr("Not checked yet");
      sig_detail_ = owner_->tr("Checked when the game starts");
      sig_count_.clear();
      sig_note_ = owner_->tr(
          "Not checked yet. The game checks every signature Xerces needs when "
          "it starts; the result shows here after the next launch.");
      return;
    }
    sig_value_ = std::to_string(sigs.matched) + "/" + std::to_string(sigs.total) +
                 " " + owner_->tr("matched");
    sig_detail_ = sigs.missing.empty()
                      ? owner_->tr("All found at the last start")
                      : std::to_string(sigs.missing.size()) + " " +
                            owner_->tr("missing");
    sig_count_ = sig_value_;
    sig_rows_.push_back({owner_->tr("Matched"),
                         std::to_string(sigs.matched) + " " + owner_->tr("of") +
                             " " + std::to_string(sigs.total),
                         true, !sigs.missing.empty()});
    sig_rows_.push_back({owner_->tr("Game build"),
                         sigs.game_build.empty() ? owner_->tr("Unknown")
                                                 : sigs.game_build,
                         true, false});
    sig_rows_.push_back({owner_->tr("Checked"),
                         sigs.checked.empty() ? owner_->tr("Unknown")
                                              : sigs.checked,
                         true, false});
    for (const std::string &name : sigs.missing) {
      missing_.push_back({name});
    }
  }

  // ---------------------------------------------------------------- catalog
  void refresh_catalog() {
    services &svc = owner_->svc();
    const catalog_state &catalog = svc.catalog();
    catalog_rows_.clear();
    diags_.clear();
    diag_more_.clear();

    std::size_t mods = 0, maps = 0, legacy = 0, broken = 0;
    std::map<std::string, std::size_t> from;
    for (std::size_t i = 0; i < catalog.items.size(); ++i) {
      const auto &item = catalog.items[i];
      (item.info.kind == xerces_core::content_kind::map ? maps : mods) += 1;
      legacy += item.info.legacy ? 1 : 0;
      broken += item.valid_manifest ? 0 : 1;
      if (i < catalog.sources.size()) {
        ++from[catalog.sources[i]];
      }
    }
    if (catalog.scanning) {
      catalog_value_ = catalog.total ? std::to_string(catalog.done) + "/" +
                                           std::to_string(catalog.total)
                                     : owner_->tr("Reading...");
      catalog_detail_ = catalog.current;
    } else if (!catalog.scanned) {
      catalog_value_ = owner_->tr("Not read yet");
      catalog_detail_.clear();
    } else {
      catalog_value_ = std::to_string(catalog.items.size()) + " " +
                       owner_->tr(catalog.items.size() == 1 ? "item" : "items");
      catalog_detail_ = std::to_string(mods) + " " + owner_->tr("mods") + ", " +
                        std::to_string(maps) + " " + owner_->tr("maps");
    }
    catalog_count_ = catalog.scanning ? owner_->tr("reading")
                     : catalog.scanned
                         ? std::to_string(catalog.items.size()) + " " +
                               owner_->tr(catalog.items.size() == 1 ? "item"
                                                                    : "items")
                         : std::string();

    const auto row = [this](const std::string &name, std::string value,
                            bool mono = true, bool warn = false) {
      catalog_rows_.push_back({owner_->tr(name), std::move(value), mono, warn});
    };
    if (catalog.scanning) {
      row("Reading",
          (catalog.total ? std::to_string(catalog.done) + "/" +
                               std::to_string(catalog.total) + "  "
                         : std::string()) +
              catalog.current);
    }
    if (catalog.scanned) {
      row("Items", std::to_string(catalog.items.size()));
      row("Mods", std::to_string(mods));
      row("Maps", std::to_string(maps));
      row("Without manifest", std::to_string(legacy));
      if (broken > 0) {
        // Folders whose manifest has no usable id are left out entirely; they
        // show up only in the diagnostics below.
        row("Kept with manifest errors", std::to_string(broken), true, true);
      }
      std::string sources;
      for (const char *name : {"workshop", "mods", "usermaps"}) {
        const auto it = from.find(name);
        sources += (sources.empty() ? "" : ", ") + std::string(name) + " " +
                   std::to_string(it == from.end() ? 0 : it->second);
      }
      row("From", sources);
      row("Scan time", seconds_text(catalog.seconds));
      row("Hash cache", std::to_string(catalog.cache_hits) + " " +
                            owner_->tr("hits") + ", " +
                            std::to_string(catalog.cache_misses) + " " +
                            owner_->tr("misses"));
      row("Hashed", xerces_core::format_bytes(catalog.bytes_hashed));
    } else if (!catalog.scanning) {
      row("Items", owner_->tr(svc.game().dir ? "Not read yet"
                                              : "No mod folders: the game was not found"),
          false);
    }

    // Worst first, then in scan order.
    std::vector<const xerces_core::diagnostic *> sorted;
    for (const auto &item : catalog.diags.items()) {
      sorted.push_back(&item);
    }
    std::ranges::stable_sort(sorted, [](const auto *a, const auto *b) {
      return severity_rank(a->level) < severity_rank(b->level);
    });
    const std::size_t errors =
        catalog.diags.count(xerces_core::severity::error);
    const std::size_t warnings =
        catalog.diags.count(xerces_core::severity::warning);
    const std::size_t infos = catalog.diags.count(xerces_core::severity::info);
    diag_count_ = std::to_string(errors) + " " + owner_->tr("errors") + " \xC2\xB7 " +
                  std::to_string(warnings) + " " + owner_->tr("warnings") +
                  " \xC2\xB7 " + std::to_string(infos) + " " + owner_->tr("notes");
    for (std::size_t i = 0; i < sorted.size() && i < max_diag_rows; ++i) {
      const auto &item = *sorted[i];
      diags_.push_back(
          {owner_->tr(item.level == xerces_core::severity::error     ? "Error"
                      : item.level == xerces_core::severity::warning ? "Warning"
                                                                     : "Info"),
           item.code, item.message,
           item.level == xerces_core::severity::error,
           item.level == xerces_core::severity::warning});
    }
    // Copies, not pointers: a finished scan replaces the catalog before
    // this refresh runs, and a click can land in between.
    shown_diag_codes_.clear();
    shown_diag_text_.clear();
    for (std::size_t i = 0; i < sorted.size() && i < max_diag_rows; ++i) {
      shown_diag_codes_.push_back(sorted[i]->code);
      shown_diag_text_.push_back(diag_line(*sorted[i]));
    }
    if (sorted.size() > max_diag_rows) {
      diag_more_ = "+" + std::to_string(sorted.size() - max_diag_rows) + " " +
                   owner_->tr("more in the copied report");
    }
    diag_note_ = catalog.scanned
                     ? owner_->tr("The last scan found nothing to report.")
                     : owner_->tr("Shown after the mods have been read.");
  }

  static std::string diag_line(const xerces_core::diagnostic &item) {
    return "[" + std::string(xerces_core::to_string(item.level)) + "] " +
           item.code + (item.path.empty() ? "" : " (" + item.path + ")") +
           ": " + item.message;
  }

  // ------------------------------------------------------------ last launch
  void refresh_launch() {
    services &svc = owner_->svc();
    launch_rows_.clear();
    launch_stack_.clear();
    launch_note_.clear();
    const auto journal = svc.last_journal();
    const xerces_core::safe_mode_decision decision = svc.safe_mode();
    safe_on_ = decision.safe_mode;
    safe_text_ = decision.message;
    const bool running = svc.game_running();
    if (!journal) {
      launch_value_ = owner_->tr("None yet");
      launch_detail_ = owner_->tr("No launch recorded");
      launch_count_.clear();
      launch_note_ = owner_->tr(
          "No launch recorded yet. Xerces writes a journal at every launch, "
          "so a crash can be traced to the mods that were loaded.");
      return;
    }
    launch_value_ = running ? owner_->tr("Running")
                    : decision.safe_mode
                        ? owner_->tr("Crashed")
                        : owner_->tr(phase_label(journal->phase));
    launch_detail_ = journal->started;
    launch_count_ = journal->stack.empty()
                        ? owner_->tr("vanilla")
                        : std::to_string(journal->stack.size()) + " " +
                              owner_->tr(journal->stack.size() == 1 ? "mod"
                                                                    : "mods");
    const auto row = [this](const std::string &name, std::string value,
                            bool mono = true, bool warn = false) {
      launch_rows_.push_back({owner_->tr(name), std::move(value), mono, warn});
    };
    row("Plan", journal->plan_id.empty() ? owner_->tr("None (vanilla)")
                                         : journal->plan_id);
    for (std::size_t i = 0; i < journal->stack.size(); ++i) {
      launch_stack_.push_back({two_digits(i + 1) + "  " + journal->stack[i]});
    }
    if (journal->stack.empty()) {
      row("Mods", owner_->tr("None"), false);
    }
    row("Phase",
        std::string(xerces_core::to_string(journal->phase)) +
            (running ? "  (" + owner_->tr("game running") + ")" : std::string()),
        true, decision.safe_mode);
    row("Started", journal->started.empty() ? "-" : journal->started);
    row("Updated", journal->updated.empty() ? "-" : journal->updated);
    row("Mods off", owner_->tr(journal->safe_mode ? "Yes (safe mode)" : "No"),
        false);
    row("Safe mode next",
        owner_->tr(decision.safe_mode ? "Suggested" : "Not needed"), false,
        decision.safe_mode);
  }

  // ----------------------------------------------------------------- files
  void refresh_files() {
    const launcher_paths &paths = owner_->svc().paths();
    log_rows_.clear();
    crashes_.clear();
    crash_more_.clear();

    std::size_t sessions = 0;
    std::error_code ec;
    for (fs::directory_iterator it(paths.logs, ec), end; !ec && it != end;
         it.increment(ec)) {
      if (it->path().extension() == ".jsonl") {
        ++sessions;
      }
    }
    log_count_ = std::to_string(sessions) + " " +
                 owner_->tr(sessions == 1 ? "session" : "sessions");
    const std::string current = native(log_path());
    log_rows_.push_back({owner_->tr("This session"),
                         current.empty() ? owner_->tr("Not written (no log file)")
                                         : current,
                         true, current.empty()});
    log_rows_.push_back({owner_->tr("Folder"), utf8(paths.logs), true, false});
    log_rows_.push_back(
        {owner_->tr("Journal"),
         fs::exists(paths.journal_file(), ec) ? utf8(paths.journal_file())
                                              : owner_->tr("None yet"),
         true, false});

    crash_entries_ = xerces_core::list_crash_entries(paths.crash);
    for (std::size_t i = 0; i < crash_entries_.size() && i < max_crash_rows;
         ++i) {
      const auto &entry = crash_entries_[i];
      crashes_.push_back({entry.name + (entry.folder ? "\\" : ""),
                          xerces_core::format_bytes(entry.size),
                          xerces_core::format_unix_time(entry.modified)});
    }
    crash_count_ = crash_entries_.empty()
                       ? owner_->tr("none")
                       : std::to_string(crash_entries_.size());
    if (crash_entries_.size() > max_crash_rows) {
      crash_more_ = "+" + std::to_string(crash_entries_.size() - max_crash_rows) +
                    " " + owner_->tr("more in the folder");
    }
  }

  // ---------------------------------------------------------------- report
  // The copied report stays in English whatever the UI language: it is read
  // by whoever triages the bug, and one language keeps reports comparable.
  std::string build_report() {
    using xerces_core::report_section;
    services &svc = owner_->svc();
    const launcher_paths &paths = svc.paths();
    const game_state &game = svc.game();
    const auto &config = svc.config();
    std::vector<report_section> sections;
    std::error_code ec;

    report_section xerces{"Xerces", {}, {}};
    xerces.fields = {
        {"Version", services::version()},
        {"Build", paths.dev_data ? "development checkout" : "release"},
        {"Dry launch", svc.dry_launch() ? "yes (the game is never started)" : "no"},
        {"Windows", windows_version()},
        {"Home folder", utf8(paths.home)},
        {"Data folder", utf8(paths.data)},
        {"Compat folder", utf8(paths.compat)},
        {"Language", owner_->language()},
        {"Theme", config.theme},
        {"UI scale", config.ui_scale_percent == 0
                         ? std::string("follows Windows")
                         : std::to_string(config.ui_scale_percent) + " %"},
        {"Reduced motion", config.reduced_motion ? "on" : "off"},
        {"Settings file", svc.config_error().empty() ? "read" : svc.config_error()},
    };
    sections.push_back(std::move(xerces));

    report_section build{"Game", {}, {}};
    build.fields.push_back(
        {"Folder", !game.searched ? "not searched yet"
                   : game.dir     ? utf8(*game.dir)
                                  : "not found"});
    if (game.dir) {
      build.fields.push_back(
          {"Steam build",
           game.steam_build ? std::to_string(*game.steam_build) : "unknown"});
    }
    if (game.exe) {
      const auto &exe = *game.exe;
      build.fields.push_back({"Exe size", xerces_core::format_count(exe.size) +
                                              " bytes (" +
                                              xerces_core::format_bytes(exe.size) +
                                              ")"});
      build.fields.push_back({"Checksum", xerces_core::format_hex32(exe.checksum)});
      build.fields.push_back({"Link time",
                              xerces_core::format_hex32(exe.link_time) + " (" +
                                  xerces_core::format_unix_time(exe.link_time) +
                                  ")"});
      build.fields.push_back({"SHA-256", exe.sha256});
    }
    if (game.preflight) {
      const auto &check = *game.preflight;
      build.fields.push_back({"Pre-flight", xerces_core::to_string(check.verdict)});
      build.fields.push_back({"Message", check.message});
      build.fields.push_back(
          {"Matched build",
           check.build ? check.build->name + " (" +
                             xerces_core::to_string(check.build->status) + ")"
                       : "none"});
    }
    if (!game.error.empty()) {
      build.fields.push_back({"Problem", game.error});
    }
    if (game.dir) {
      build.fields.push_back({"Workshop folder", utf8(game.workshop_dir)});
      build.fields.push_back({"Mods folder", utf8(game.mods_dir)});
      build.fields.push_back({"Usermaps folder", utf8(game.usermaps_dir)});
    }
    build.fields.push_back({"Client", utf8(paths.client)});
    build.fields.push_back(
        {"Client file", fs::exists(paths.client, ec) ? "present" : "missing"});
    build.fields.push_back({"Game running", svc.game_running() ? "yes" : "no"});
    if (!game.dir) {
      for (const std::string &place : game.tried) {
        build.lines.push_back("Looked in: " + place);
      }
    }
    sections.push_back(std::move(build));

    const signature_state &sigs = svc.signatures();
    report_section signatures{"Signatures", {}, {}};
    if (!sigs.known) {
      signatures.fields.push_back(
          {"Report", "none yet (checked when the game starts)"});
    } else {
      signatures.fields = {
          {"Matched", std::to_string(sigs.matched) + " of " +
                          std::to_string(sigs.total)},
          {"Game build", sigs.game_build},
          {"Checked", sigs.checked},
      };
      for (const std::string &name : sigs.missing) {
        signatures.lines.push_back("Missing: " + name);
      }
    }
    sections.push_back(std::move(signatures));

    const catalog_state &catalog = svc.catalog();
    report_section mods{"Mods found", {}, {}};
    std::size_t mod_count = 0, map_count = 0, legacy = 0, broken = 0;
    for (const auto &item : catalog.items) {
      (item.info.kind == xerces_core::content_kind::map ? map_count : mod_count) +=
          1;
      legacy += item.info.legacy ? 1 : 0;
      broken += item.valid_manifest ? 0 : 1;
    }
    mods.fields = {
        {"State", catalog.scanning  ? "reading (" + std::to_string(catalog.done) +
                                          "/" + std::to_string(catalog.total) + ")"
                  : catalog.scanned ? "read"
                                    : "not read"},
        {"Items", std::to_string(catalog.items.size())},
        {"Mods", std::to_string(mod_count)},
        {"Maps", std::to_string(map_count)},
        {"Without manifest", std::to_string(legacy)},
        {"Kept with manifest errors", std::to_string(broken)},
        {"Scan time", seconds_text(catalog.seconds)},
        {"Hash cache", std::to_string(catalog.cache_hits) + " hits, " +
                           std::to_string(catalog.cache_misses) + " misses"},
        {"Hashed", xerces_core::format_bytes(catalog.bytes_hashed)},
    };
    for (std::size_t i = 0; i < catalog.items.size(); ++i) {
      const auto &item = catalog.items[i];
      mods.lines.push_back(
          (i < catalog.sources.size() ? catalog.sources[i] : std::string("?")) +
          "  " + item.info.id + "  " +
          (item.info.legacy ? std::string("legacy") : item.info.ver.to_string()) +
          "  " + xerces_core::to_string(item.info.kind) + "  " +
          xerces_core::short_hash(item.content_hash) + "  " + item.folder);
    }
    for (const auto &item : catalog.diags.items()) {
      mods.lines.push_back(diag_line(item));
    }
    sections.push_back(std::move(mods));

    // The plan each mode would load now: mods in order with versions and
    // hashes (5.10), so a report shows exactly what was asked for.
    for (const char *mode : {"zm", "mp", "cp"}) {
      const auto profile = config.profiles.find(mode);
      if (profile == config.profiles.end() || profile->second.mods.empty()) {
        continue;
      }
      const xerces_core::plan plan = svc.resolve(mode);
      report_section section{std::string("Plan (") + mode + ")", {}, {}};
      section.fields = {
          {"Enabled", std::to_string(profile->second.mods.size())},
          {"Plan id", plan.plan_id},
          {"Content id", plan.content_id},
          {"Zones added", std::to_string(plan.zones_added)},
          {"Map", plan.map ? plan.map->id : "-"},
          {"Result", plan.ok() ? "ok" : "has problems"},
      };
      for (std::size_t i = 0; i < plan.stack.size(); ++i) {
        const auto &entry = plan.stack[i];
        section.lines.push_back(
            two_digits(i + 1) + "  " + entry.id + "  " +
            (entry.legacy ? std::string("legacy") : entry.ver.to_string()) +
            "  " + xerces_core::short_hash(entry.content_hash) +
            (entry.requested ? "" : "  (dependency)"));
      }
      for (const auto &item : plan.diags.items()) {
        section.lines.push_back(diag_line(item));
      }
      sections.push_back(std::move(section));
    }

    report_section launch{"Last launch", {}, {}};
    if (const auto journal = svc.last_journal()) {
      launch.fields = {
          {"Plan id", journal->plan_id.empty() ? "none (vanilla)" : journal->plan_id},
          {"Phase", xerces_core::to_string(journal->phase)},
          {"Started", journal->started},
          {"Updated", journal->updated},
          {"Mods off", journal->safe_mode ? "yes" : "no"},
      };
      for (std::size_t i = 0; i < journal->stack.size(); ++i) {
        launch.lines.push_back(two_digits(i + 1) + "  " + journal->stack[i]);
      }
    } else {
      launch.fields.push_back({"Journal", "none yet"});
    }
    const xerces_core::safe_mode_decision decision = svc.safe_mode();
    launch.fields.push_back(
        {"Safe mode", decision.safe_mode ? "suggested: " + decision.message
                                         : "not needed"});
    sections.push_back(std::move(launch));

    report_section files{"Logs", {}, {}};
    files.fields = {
        {"This session", native(log_path())},
        {"Logs folder", utf8(paths.logs)},
        {"Journal", utf8(paths.journal_file())},
        {"Crash folder", utf8(paths.crash)},
        {"Crash bundles", std::to_string(crash_entries_.size())},
    };
    for (const auto &entry : crash_entries_) {
      files.lines.push_back(entry.name + (entry.folder ? "\\" : "") + "  " +
                            xerces_core::format_bytes(entry.size) + "  " +
                            xerces_core::format_unix_time(entry.modified));
    }
    sections.push_back(std::move(files));

    return xerces_core::format_report(sections, xerces_core::profile_folder());
  }

  // --------------------------------------------------------------- actions
  void act(const std::string &action) {
    log_info("launcher.diagnostics.action", action);
    services &svc = owner_->svc();
    if (action == "copy") {
      refresh_files(); // the crash list as it is now, on screen too
      handle_.DirtyAllVariables();
      const std::string report = build_report();
      owner_->copy_text(report);
      log_info("launcher.diagnostics.copied",
               std::to_string(report.size()) + " bytes",
               "{\"bytes\":" + std::to_string(report.size()) + "}");
      owner_->notify(owner_->tr(
          "Report copied. Paste it into your bug report; your user folder "
          "shows as %USERPROFILE%."));
    } else if (action == "refresh") {
      svc.reload_signatures();
      svc.refresh(true);
      refresh_asked_ = true;
      refresh_launch();
      refresh_files();
      handle_.DirtyAllVariables();
      owner_->notify(owner_->tr("Checking the game and reading the mods again."));
    } else if (action == "verify") {
      std::string body = owner_->tr(
          "Steam will check every Black Ops III file and download any that "
          "are missing or changed. Xerces does not change game files itself. "
          "The check can take several minutes; press Refresh when Steam has "
          "finished.");
      if (svc.game_running()) {
        body += " " + owner_->tr("Close the game first: it is still running.");
      }
      owner_->dialog("Verify with Steam", body,
                     {"Verify with Steam", "Cancel"}, [this](int choice) {
                       if (choice != 0) {
                         log_info("launcher.diagnostics.verify", "cancelled");
                         return;
                       }
                       const std::string url =
                           "steam://validate/" +
                           std::string(xerces_core::bo3_app_id);
                       if (owner_->open_url(url)) {
                         owner_->notify(owner_->tr(
                             "Steam is checking the game files."));
                       } else {
                         owner_->notify(
                             owner_->tr("Steam could not be opened. Is it "
                                        "installed?"),
                             true);
                       }
                     });
    } else if (action == "reread signatures") {
      // The game writes the report when it starts, also when it was started
      // without Xerces; the launcher otherwise reads it only at its own start
      // and after a game it launched exits.
      svc.reload_signatures();
      owner_->notify(svc.signatures().known
                         ? owner_->tr("Signature report read again.")
                         : owner_->tr("No signature report yet: start the game "
                                      "once."));
    } else if (action == "open game") {
      if (!svc.game().dir || !owner_->open_folder(*svc.game().dir)) {
        owner_->notify(owner_->tr("The game folder could not be opened."), true);
      }
    } else if (action == "rescan") {
      svc.start_scan();
      owner_->notify(owner_->tr("Reading the mods again."));
    } else if (action == "open logs") {
      if (!owner_->open_folder(svc.paths().logs)) {
        owner_->notify(owner_->tr("The logs folder could not be opened."), true);
      }
    } else if (action == "copy log path") {
      const std::string path = native(log_path());
      if (path.empty()) {
        owner_->notify(owner_->tr("This session has no log file."), true);
        return;
      }
      owner_->copy_text(path);
      owner_->notify(owner_->tr("Log path copied."));
    } else if (action == "open crash") {
      std::error_code ec;
      fs::create_directories(svc.paths().crash, ec);
      if (!owner_->open_folder(svc.paths().crash)) {
        owner_->notify(owner_->tr("The crash folder could not be opened."), true);
      }
    }
  }

  void copy_diag(int index) {
    if (index < 0 || std::size_t(index) >= shown_diag_text_.size()) {
      return;
    }
    owner_->copy_text(xerces_core::redact_profile(shown_diag_text_[index],
                                                  xerces_core::profile_folder()));
    log_info("launcher.diagnostics.copied_line", shown_diag_codes_[index]);
    owner_->notify(owner_->tr("Copied:") + " " + shown_diag_codes_[index]);
  }

  app *owner_ = nullptr;
  Rml::DataModelHandle handle_;
  std::uint64_t seen_game_ = 0, seen_config_ = 0, seen_signatures_ = 0,
                seen_catalog_ = 0, seen_journal_ = 0;
  double catalog_at_ = -1;
  // A pending Up/Down from key(): scroll if focus did not move (-1, 0, 1).
  int nudge_ = 0;
  Rml::Element *nudge_from_ = nullptr;
  std::vector<std::string> shown_diag_codes_, shown_diag_text_;
  std::vector<xerces_core::crash_entry> crash_entries_;

  bool busy_ = false;
  bool refresh_asked_ = false; // say so when the player's refresh finishes
  Rml::String refresh_label_ = "Refresh";
  Rml::String build_value_, build_detail_, build_note_, build_count_;
  bool build_ok_ = true, game_found_ = false;
  Rml::Vector<kv_row> build_rows_;
  Rml::Vector<text_row> tried_;
  Rml::String sig_value_, sig_detail_, sig_count_, sig_note_;
  bool sig_warn_ = false;
  Rml::Vector<kv_row> sig_rows_;
  Rml::Vector<text_row> missing_;
  Rml::String catalog_value_, catalog_detail_, catalog_count_;
  Rml::Vector<kv_row> catalog_rows_;
  Rml::Vector<diag_row> diags_;
  Rml::String diag_count_, diag_note_, diag_more_;
  Rml::String launch_value_, launch_detail_, launch_count_, launch_note_;
  Rml::Vector<kv_row> launch_rows_;
  Rml::Vector<text_row> launch_stack_;
  bool safe_on_ = false;
  Rml::String safe_text_;
  Rml::String log_count_;
  Rml::Vector<kv_row> log_rows_;
  Rml::Vector<file_row> crashes_;
  Rml::String crash_count_, crash_more_;
};
} // namespace

XERCES_VIEW(diagnostics_view, "diagnostics", 70, true);
} // namespace xerces_launcher
