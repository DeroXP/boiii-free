// Boot (docs/DESIGN.md 7.3): the mark (already drawn by the window's native
// paint) with the word and the status fading in, while the game is found and
// its build checked on the worker thread; then First run
// (no game folder yet, or never finished) or Home. It never waits on the
// network, and gives up waiting after three seconds.

#include <RmlUi/Core.h>

#include "../app.hpp"

namespace xerces_launcher {
namespace {
class boot_view final : public view {
public:
  const char *id() const override { return "boot"; }
  std::string document() const override { return "boot.rml"; }

  bool bind(Rml::Context *context, app &owner) override {
    owner_ = &owner;
    Rml::DataModelConstructor model = context->CreateDataModel("boot");
    if (!model) {
      return false;
    }
    model.Bind("step", &step_);
    handle_ = model.GetModelHandle();
    return true;
  }

  void shown() override { shown_at_ = owner_->now(); }

  Rml::Element *default_focus() override { return nullptr; }

  void update(double now) override {
    if (owner_->current() != "boot" || !doc) {
      return;
    }
    const double elapsed = now - shown_at_;
    // The mark is already there (the native paint); the word and the status
    // fade in once frames are on screen (the first one waits for the GPU).
    if (!faded_ && ++frames_ > 2) {
      doc->SetClass("shown", true);
      faded_ = true;
    }

    const services &svc = owner_->svc();
    Rml::String step;
    if (!svc.game().searched) {
      step = owner_->tr("Finding Black Ops III");
    } else if (!svc.game().dir) {
      step = owner_->tr("Black Ops III not found");
    } else {
      step = owner_->tr("Checking the game build");
    }
    if (step != step_) {
      step_ = step;
      handle_.DirtyVariable("step");
    }

    const bool ready = svc.game().searched && elapsed >= 0.65;
    if (ready || elapsed >= 3.0) {
      const auto &config = svc.config();
      const bool first_run =
          owner_->find("first_run") &&
          (!config.first_run_done || !svc.game().dir);
      std::string next = first_run ? "first_run" : "home";
      if (!owner_->opts().start_view.empty()) {
        next = owner_->opts().start_view;
      }
      owner_->show(next);
    }
  }

private:
  app *owner_ = nullptr;
  Rml::DataModelHandle handle_;
  Rml::String step_;
  double shown_at_ = 0;
  bool faded_ = false;
  int frames_ = 0;
};
} // namespace

XERCES_VIEW(boot_view, "boot", 0, false);
} // namespace xerces_launcher
