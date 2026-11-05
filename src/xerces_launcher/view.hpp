#pragma once

// One launcher screen (DESIGN.md 7.3). A view owns a data model and one RML
// document, data/xerces/ui/views/<id>.rml, whose body uses the "shell"
// template (header with the tabs, content, footer), so the header and the
// screen share one document and spatial navigation moves between them.
//
// Views register themselves with XERCES_VIEW in their own .cpp file; the app
// creates them in `order`, loads their documents and shows one at a time.

#include <memory>
#include <string>
#include <vector>

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Input.h>

namespace Rml {
class Context;
class Element;
class ElementDocument;
} // namespace Rml

namespace xerces_launcher {
class app;

class view {
public:
  virtual ~view() = default;

  // "home": the tab button is #tab-home, the document views/home.rml, the
  // data model "home" (by convention).
  virtual const char *id() const = 0;
  virtual std::string document() const {
    return std::string("views/") + id() + ".rml";
  }

  // Creates the data model, before the document loads (the body's
  // data-model attribute needs it). False stops the launcher.
  virtual bool bind(Rml::Context *context, app &owner) = 0;
  // After the document loaded: look up elements, add event listeners.
  virtual void loaded(Rml::ElementDocument *) {}
  virtual void shown() {}
  virtual void hidden() {}
  // Every frame, shown or not: compare the services' revision counters and
  // refresh the model when they changed. now is seconds since start.
  virtual void update(double /*now*/) {}
  // Where focus goes when the view is shown. Default: the first element with
  // the class "default-focus", else the first button in #content.
  virtual Rml::Element *default_focus();
  // Keys while the view is current, before the app's own map (Q/E tabs, /
  // search, Esc). Return true when the view used the key.
  virtual bool key(Rml::Input::KeyIdentifier, int /*modifiers*/) { return false; }
  // The search field "/" and Y focus; default #search.
  virtual Rml::Element *search_field();

  Rml::ElementDocument *doc = nullptr; // set by the app
};

using view_factory = std::unique_ptr<view> (*)();

struct view_info {
  const char *id;
  int order;     // tab order (and load order)
  bool tab;      // has a header tab (#tab-<id>)
  view_factory create;
};

std::vector<view_info> &view_registry();

struct view_registration {
  view_registration(const char *id, int order, bool tab, view_factory create) {
    view_registry().push_back({id, order, tab, create});
  }
};
} // namespace xerces_launcher

#define XERCES_VIEW_CONCAT_INNER(a, b) a##b
#define XERCES_VIEW_CONCAT(a, b) XERCES_VIEW_CONCAT_INNER(a, b)
// XERCES_VIEW(home_view, "home", 10, true)
#define XERCES_VIEW(type, id, order, tab)                                      \
  static ::xerces_launcher::view_registration XERCES_VIEW_CONCAT(              \
      xerces_view_registration_, __LINE__)(                                    \
      id, order, tab, []() -> std::unique_ptr<::xerces_launcher::view> {       \
        return std::make_unique<type>();                                       \
      })
