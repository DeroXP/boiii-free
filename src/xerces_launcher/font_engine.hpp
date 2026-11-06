#pragma once

// The launcher's font engine (docs/DESIGN.md 7.1). A build made with HarfBuzz
// (premake finds third_party\harfbuzz\harfbuzz-15.0.0 and defines
// XERCES_LAUNCHER_HARFBUZZ) shapes text with RmlUi 6.3's HarfBuzz font engine
// (MIT), built from RmlUi's Samples/basic/harfbuzz except
// FontFaceHandleHarfBuzz.cpp, whose changed copy is in
// src/xerces_rmlui_harfbuzz: OpenType (GPOS) kerning, ligatures and the scripts
// that need shaping. Other builds keep RmlUi's own FreeType engine, which reads
// only the legacy kern table.

#include <string>

#include <xerces_core/text_language.hpp>

namespace xerces_launcher {
// Makes the engine RmlUi's font engine; call before Rml::Initialise.
void install_font_engine();

// Which engine install_font_engine() set, for the log: "harfbuzz" or
// "freetype", and HarfBuzz's version ("15.0.0") or an empty string.
const char *font_engine_name();
std::string harfbuzz_version();

// Tells the engine the script and direction of the documents' language (the
// lang the documents inherit from the context's root is language.tag).
// Nothing to tell RmlUi's FreeType engine, which does not shape.
void register_text_language(const xerces_core::text_language &language);
} // namespace xerces_launcher
