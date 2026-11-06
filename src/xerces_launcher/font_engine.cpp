#include "font_engine.hpp"

#ifdef XERCES_LAUNCHER_HARFBUZZ
#include <hb.h>
#include <RmlUi/Core/Core.h>

// RmlUi's sample header (Samples/basic/harfbuzz/src); it brings a few
// Rml names into the global namespace, so only this file includes it.
#include <FontEngineInterfaceHarfBuzz.h>
#endif

namespace xerces_launcher {
#ifdef XERCES_LAUNCHER_HARFBUZZ
namespace {
// RmlUi holds on to the engine until Rml::Shutdown, so it lives as long as
// the process.
FontEngineInterfaceHarfBuzz &engine() {
  static FontEngineInterfaceHarfBuzz instance;
  return instance;
}
} // namespace

void install_font_engine() { Rml::SetFontEngineInterface(&engine()); }

const char *font_engine_name() { return "harfbuzz"; }

std::string harfbuzz_version() { return hb_version_string(); }

void register_text_language(const xerces_core::text_language &language) {
  // An empty script leaves it to the engine, which then takes the script of
  // each text from its first letter.
  if (language.tag.empty() || language.script.empty()) {
    return;
  }
  engine().RegisterLanguage(language.tag, language.script,
                            language.right_to_left
                                ? TextFlowDirection::RightToLeft
                                : TextFlowDirection::LeftToRight);
}
#else
void install_font_engine() {} // Rml::Initialise makes its FreeType engine

const char *font_engine_name() { return "freetype"; }

std::string harfbuzz_version() { return {}; }

void register_text_language(const xerces_core::text_language &) {}
#endif
} // namespace xerces_launcher
