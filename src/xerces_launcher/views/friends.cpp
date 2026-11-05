// Friends (docs/DESIGN.md 7.3). Placeholder until the screen is built: it
// shows the frame and a short description.

#include <RmlUi/Core.h>

#include "../app.hpp"

namespace xerces_launcher {
namespace {
class friends_view final : public view {
public:
  const char *id() const override { return "friends"; }
  bool bind(Rml::Context *, app &) override { return true; }
};
} // namespace

XERCES_VIEW(friends_view, "friends", 50, true);
} // namespace xerces_launcher
