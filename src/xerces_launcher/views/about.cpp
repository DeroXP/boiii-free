// About and Legal (docs/DESIGN.md 7.3, 16.5): the version, the licence and the
// credits the third-party licences ask for. Not a tab; the footer's About link
// opens it.

#include <RmlUi/Core.h>

#include <string>

#include "../app.hpp"
#include "../font_engine.hpp"

namespace xerces_launcher {
namespace {
class about_view final : public view {
public:
  const char *id() const override { return "about"; }

  bool bind(Rml::Context *context, app &owner) override {
    owner_ = &owner;
    Rml::DataModelConstructor model = context->CreateDataModel("about");
    if (!model) {
      return false;
    }
    version_ = "Xerces " + services::version();
    model.Bind("version", &version_);
    // HarfBuzz and RmlUi's HarfBuzz engine are credited when this build has
    // them (font_engine.hpp).
    harfbuzz_ = std::string(font_engine_name()) == "harfbuzz";
    model.Bind("harfbuzz", &harfbuzz_);
    return true;
  }

  void loaded(Rml::ElementDocument *document) override {
    owner_->listen(document->GetElementById("about-licence"), "click",
                   [this](Rml::Event &) {
                     owner_->open_url("https://www.gnu.org/licenses/gpl-3.0.html");
                   });
    owner_->listen(document->GetElementById("about-home"), "click",
                   [this](Rml::Event &) { owner_->show("home"); });
  }

private:
  app *owner_ = nullptr;
  Rml::String version_;
  bool harfbuzz_ = false;
};
} // namespace

XERCES_VIEW(about_view, "about", 90, false);
} // namespace xerces_launcher
