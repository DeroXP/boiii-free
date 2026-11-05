// Welcome (docs/DESIGN.md 7.3). Placeholder until the screen is built: it
// shows the frame and a short description.

#include <RmlUi/Core.h>

#include "../app.hpp"

namespace xerces_launcher {
namespace {
class first_run_view final : public view {
public:
  const char *id() const override { return "first_run"; }
  bool bind(Rml::Context *, app &) override { return true; }
};
} // namespace

XERCES_VIEW(first_run_view, "first_run", 5, false);
} // namespace xerces_launcher
