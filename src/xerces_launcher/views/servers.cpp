// Servers (docs/DESIGN.md 7.3). Placeholder until the screen is built: it
// shows the frame and a short description.

#include <RmlUi/Core.h>

#include "../app.hpp"

namespace xerces_launcher {
namespace {
class servers_view final : public view {
public:
  const char *id() const override { return "servers"; }
  bool bind(Rml::Context *, app &) override { return true; }
};
} // namespace

XERCES_VIEW(servers_view, "servers", 40, true);
} // namespace xerces_launcher
