// Settings (docs/DESIGN.md 7.3). Placeholder until the screen is built: it
// shows the frame and a short description.

#include <RmlUi/Core.h>

#include "../app.hpp"

namespace xerces_launcher {
namespace {
class settings_view final : public view {
public:
  const char *id() const override { return "settings"; }
  bool bind(Rml::Context *, app &) override { return true; }
};
} // namespace

XERCES_VIEW(settings_view, "settings", 60, true);
} // namespace xerces_launcher
