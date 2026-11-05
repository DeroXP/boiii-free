// Boot (docs/DESIGN.md 7.3): the mark builds up (at most 900 ms) while the
// game is found and its build checked on the worker thread; then First run
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
    // One square every 150 ms: the mark is whole at 600 ms.
    const int lit = std::min(4, int(elapsed / 0.15) + 1);
    for (int i = lit_; i < lit; ++i) {
      if (Rml::Element *square = doc->GetElementById("m" + std::to_string(i))) {
        square->SetClass("on", true);
      }
    }
    lit_ = lit;

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
  int lit_ = 0;
};
} // namespace

XERCES_VIEW(boot_view, "boot", 0, false);
} // namespace xerces_launcher
