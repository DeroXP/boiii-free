#pragma once

// Text the launcher shows through app::tr() (docs/DESIGN.md 7.5) that is not
// written inside a tr() call: a message the core or the services hand to a
// screen, which translates it there. tools/i18n/extract_strings.py collects
// every string literal inside ui_text(...) into the string template, so the
// text can be translated; at run time ui_text() returns the English text as
// it is.

namespace xerces_core {
constexpr const char *ui_text(const char *english) { return english; }
} // namespace xerces_core
