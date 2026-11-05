// Diagnostics (docs/DESIGN.md 7.3). Placeholder until the screen is built: it
// shows the frame and a short description.

#include <RmlUi/Core.h>

#include "../app.hpp"

namespace xerces_launcher {
namespace {
class diagnostics_view final : public view {
public:
  const char *id() const override { return "diagnostics"; }
  bool bind(Rml::Context *, app &) override { return true; }
};
} // namespace

XERCES_VIEW(diagnostics_view, "diagnostics", 70, true);
} // namespace xerces_launcher
