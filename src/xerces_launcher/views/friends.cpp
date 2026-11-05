// Friends (docs/DESIGN.md 7.3): arrives with Xerces accounts (M5). This page
// says what is off in this build (D14) and what will come; it never talks to a
// network.

#include <RmlUi/Core.h>

#include "../app.hpp"

namespace xerces_launcher {
namespace {
class friends_view final : public view {
public:
  const char *id() const override { return "friends"; }

  bool bind(Rml::Context *, app &owner) override {
    owner_ = &owner;
    return true;
  }

  void loaded(Rml::ElementDocument *document) override {
    owner_->listen(document->GetElementById("friends-servers"), "click",
                   [this](Rml::Event &) { owner_->show("servers"); });
    owner_->listen(document->GetElementById("friends-home"), "click",
                   [this](Rml::Event &) { owner_->show("home"); });
  }

private:
  app *owner_ = nullptr;
};
} // namespace

XERCES_VIEW(friends_view, "friends", 50, true);
} // namespace xerces_launcher
