#include "text_language.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>

namespace xerces_core {
namespace {
struct language_script {
  std::string_view language; // primary subtag, lower case
  std::string_view script;   // ISO 15924
};

// Languages a translation is likely to come in, by the one script each is
// usually written in. Japanese mixes kana and kanji, so it is left out (the
// script then comes from the text).
constexpr language_script known[] = {
    {"af", "Latn"}, {"ca", "Latn"}, {"cs", "Latn"}, {"cy", "Latn"},
    {"da", "Latn"}, {"de", "Latn"}, {"en", "Latn"}, {"es", "Latn"},
    {"et", "Latn"}, {"eu", "Latn"}, {"fi", "Latn"}, {"fil", "Latn"},
    {"fr", "Latn"}, {"ga", "Latn"}, {"gl", "Latn"}, {"hr", "Latn"},
    {"hu", "Latn"}, {"id", "Latn"}, {"is", "Latn"}, {"it", "Latn"},
    {"lt", "Latn"}, {"lv", "Latn"}, {"ms", "Latn"}, {"nb", "Latn"},
    {"nl", "Latn"}, {"nn", "Latn"}, {"no", "Latn"}, {"pl", "Latn"},
    {"pt", "Latn"}, {"ro", "Latn"}, {"sk", "Latn"}, {"sl", "Latn"},
    {"sq", "Latn"}, {"sv", "Latn"}, {"sw", "Latn"}, {"tr", "Latn"},
    {"vi", "Latn"}, {"be", "Cyrl"}, {"bg", "Cyrl"}, {"kk", "Cyrl"},
    {"mk", "Cyrl"}, {"ru", "Cyrl"}, {"sr", "Cyrl"}, {"uk", "Cyrl"},
    {"el", "Grek"}, {"hy", "Armn"}, {"ka", "Geor"}, {"ar", "Arab"},
    {"fa", "Arab"}, {"ur", "Arab"}, {"he", "Hebr"}, {"hi", "Deva"},
    {"mr", "Deva"}, {"ne", "Deva"}, {"bn", "Beng"}, {"ta", "Taml"},
    {"te", "Telu"}, {"th", "Thai"}, {"km", "Khmr"}, {"lo", "Laoo"},
    {"ko", "Hang"}, {"zh", "Hani"},
};

// Scripts written from right to left.
constexpr std::string_view right_to_left[] = {"Arab", "Hebr", "Syrc",
                                              "Thaa", "Nkoo", "Adlm"};

bool is_alpha(std::string_view text) {
  return std::ranges::all_of(text, [](char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
  });
}
} // namespace

text_language text_language_for(std::string_view language) {
  if (language == "pseudo") {
    return {"en-XA", "Latn", false};
  }
  text_language out;
  out.tag = std::string(language);

  // The primary subtag, then a script subtag (four letters) if there is one.
  const std::size_t dash = language.find_first_of("-_");
  std::string primary(language.substr(0, dash));
  std::ranges::transform(primary, primary.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  for (const language_script &entry : known) {
    if (entry.language == primary) {
      out.script = std::string(entry.script);
    }
  }
  for (std::size_t at = dash; at != std::string_view::npos;) {
    const std::size_t next = language.find_first_of("-_", at + 1);
    const std::string_view subtag = language.substr(
        at + 1, next == std::string_view::npos ? next : next - at - 1);
    if (subtag.size() == 4 && is_alpha(subtag)) {
      std::string script(subtag);
      script[0] = static_cast<char>(
          std::toupper(static_cast<unsigned char>(script[0])));
      for (std::size_t i = 1; i < script.size(); ++i) {
        script[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(script[i])));
      }
      // Simplified and traditional Chinese are both Han to HarfBuzz; Japanese
      // and Korean writing mix scripts.
      if (script == "Hans" || script == "Hant") {
        script = "Hani";
      } else if (script == "Jpan" || script == "Kore") {
        script.clear();
      }
      out.script = script;
      break;
    }
    at = next;
  }
  out.right_to_left =
      std::ranges::find(right_to_left, std::string_view(out.script)) !=
      std::end(right_to_left);
  return out;
}
} // namespace xerces_core
