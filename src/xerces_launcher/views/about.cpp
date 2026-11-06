// About and Legal (docs/DESIGN.md 7.3, 16.5): the version, the notices
// GPL-3.0 section 5(d) asks for (copyright, no warranty, how to read the
// licence: the LICENSE shipped next to Xerces.exe), the commit this build's
// source is at, THIRD_PARTY_NOTICES.txt and the credits the third-party
// licences ask for. Not a tab; the footer's About link opens it.

#include <RmlUi/Core.h>

#include <string>

#include <xerces_core/version.hpp>

#include "../app.hpp"
#include "../font_engine.hpp"

namespace xerces_launcher {
namespace {
// The fork's tree at the commit this build was made from; empty when the
// build does not know its commit.
std::string source_url() {
  const std::string commit = xerces_core::xerces_commit;
  if (commit.empty()) {
    return {};
  }
  return std::string(xerces_core::xerces_source_repository) + "/tree/" + commit;
}

class about_view final : public view {
public:
  const char *id() const override { return "about"; }

  bool bind(Rml::Context *context, app &owner) override {
    owner_ = &owner;
    Rml::DataModelConstructor model = context->CreateDataModel("about");
    if (!model) {
      return false;
    }
    const launcher_paths &paths = owner.svc().paths();
    version_ = services::version_label();
    source_url_ = source_url();
    if (source_url_.empty()) {
      source_url_ = owner.tr("Unknown: this build does not know its commit.");
    }
    dirty_ = xerces_core::xerces_dirty;
    licence_path_ = paths.licence_file
                        ? xerces_core::path_utf8(*paths.licence_file)
                        : owner.tr("Not found next to Xerces.exe.");
    notices_path_ = paths.notices_file
                        ? xerces_core::path_utf8(*paths.notices_file)
                        : owner.tr("Not found next to Xerces.exe (release "
                                   "packages have it).");
    model.Bind("version", &version_);
    model.Bind("source_url", &source_url_);
    model.Bind("dirty", &dirty_);
    model.Bind("licence_path", &licence_path_);
    model.Bind("notices_path", &notices_path_);
    // HarfBuzz and RmlUi's HarfBuzz engine are credited when this build has
    // them (font_engine.hpp).
    harfbuzz_ = std::string(font_engine_name()) == "harfbuzz";
    model.Bind("harfbuzz", &harfbuzz_);
    return true;
  }

  void loaded(Rml::ElementDocument *document) override {
    owner_->listen(document->GetElementById("about-licence-file"), "click",
                   [this](Rml::Event &) {
                     open_file(owner_->svc().paths().licence_file,
                               owner_->tr("LICENSE is not next to Xerces.exe. "
                                          "Unzip the whole Xerces folder "
                                          "again."));
                   });
    owner_->listen(document->GetElementById("about-licence"), "click",
                   [this](Rml::Event &) {
                     owner_->open_url(
                         "https://www.gnu.org/licenses/gpl-3.0.html");
                   });
    owner_->listen(document->GetElementById("about-source"), "click",
                   [this](Rml::Event &) {
                     const std::string url = source_url();
                     if (url.empty() || !owner_->open_url(url)) {
                       owner_->notify(owner_->tr("Could not open the source "
                                                 "page."),
                                      true);
                     }
                   });
    owner_->listen(document->GetElementById("about-notices"), "click",
                   [this](Rml::Event &) {
                     open_file(owner_->svc().paths().notices_file,
                               owner_->tr("THIRD_PARTY_NOTICES.txt is not next "
                                          "to Xerces.exe. Release packages "
                                          "have it."));
                   });
    owner_->listen(document->GetElementById("about-home"), "click",
                   [this](Rml::Event &) { owner_->show("home"); });
  }

private:
  void open_file(const std::optional<fs::path> &file,
                 const std::string &missing) {
    if (!file || !owner_->open_text_file(*file)) {
      owner_->notify(missing, true);
    }
  }

  app *owner_ = nullptr;
  Rml::String version_;
  Rml::String source_url_;
  bool dirty_ = false;
  Rml::String licence_path_;
  Rml::String notices_path_;
  bool harfbuzz_ = false;
};
} // namespace

XERCES_VIEW(about_view, "about", 90, false);
} // namespace xerces_launcher
