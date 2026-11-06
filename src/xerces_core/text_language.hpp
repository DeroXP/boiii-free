#pragma once

// The language and script the launcher's text is shaped in (DESIGN.md 7.1,
// 7.5). The launcher sets each document's `lang` attribute to `tag`, and its
// HarfBuzz font engine shapes the text of that language with `script` and its
// direction, so the font's OpenType features (kerning, ligatures, a language's
// own letter forms) apply as the font's makers meant them.

#include <string>
#include <string_view>

namespace xerces_core {
struct text_language {
  // BCP 47 tag for the documents' lang attribute: the launcher language as
  // configured, or "en-XA" (the accented English pseudo-locale) for "pseudo".
  std::string tag;
  // ISO 15924 code of the script the language is written in ("Latn"). Empty
  // for a language this table does not know or one written in several scripts
  // (Japanese); HarfBuzz then takes the script from each text's first letter.
  std::string script;
  bool right_to_left = false;
};

// For the launcher's language setting: "en", "pseudo", or a translation's
// name in data/xerces/i18n ("de", "pt-BR", "sr-Latn"). A script subtag in the
// name wins over the language's usual script.
text_language text_language_for(std::string_view language);
} // namespace xerces_core
