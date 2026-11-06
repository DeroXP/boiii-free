// The language and script the launcher shapes its text in (DESIGN.md 7.1,
// 7.5): English, the pseudo-locale and the translations that may come.

#include "harness.hpp"

#include <xerces_core/text_language.hpp>

using namespace xerces_core;

TEST_CASE("text language: English and the pseudo-locale are Latin") {
  const text_language english = text_language_for("en");
  CHECK_EQ(english.tag, std::string("en"));
  CHECK_EQ(english.script, std::string("Latn"));
  CHECK(!english.right_to_left);

  // Accented English, tagged as the usual pseudo-locale so a font's English
  // features apply.
  const text_language pseudo = text_language_for("pseudo");
  CHECK_EQ(pseudo.tag, std::string("en-XA"));
  CHECK_EQ(pseudo.script, std::string("Latn"));
  CHECK(!pseudo.right_to_left);
}

TEST_CASE("text language: translations by their usual script") {
  CHECK_EQ(text_language_for("de").script, std::string("Latn"));
  CHECK_EQ(text_language_for("pt-BR").script, std::string("Latn"));
  CHECK_EQ(text_language_for("pt-BR").tag, std::string("pt-BR"));
  CHECK_EQ(text_language_for("PL").script, std::string("Latn"));
  CHECK_EQ(text_language_for("ru").script, std::string("Cyrl"));
  CHECK_EQ(text_language_for("el").script, std::string("Grek"));
  CHECK_EQ(text_language_for("ko").script, std::string("Hang"));
  CHECK_EQ(text_language_for("zh").script, std::string("Hani"));
}

TEST_CASE("text language: Arabic and Hebrew run right to left") {
  const text_language arabic = text_language_for("ar");
  CHECK_EQ(arabic.script, std::string("Arab"));
  CHECK(arabic.right_to_left);
  CHECK(text_language_for("he").right_to_left);
  CHECK(text_language_for("fa").right_to_left);
  CHECK(!text_language_for("hi").right_to_left);
}

TEST_CASE("text language: a script subtag wins") {
  CHECK_EQ(text_language_for("sr").script, std::string("Cyrl"));
  CHECK_EQ(text_language_for("sr-Latn").script, std::string("Latn"));
  CHECK_EQ(text_language_for("sr_latn").script, std::string("Latn"));
  CHECK_EQ(text_language_for("zh-Hant-TW").script, std::string("Hani"));
  CHECK_EQ(text_language_for("az-Cyrl").script, std::string("Cyrl"));
  // Region subtags are not scripts.
  CHECK_EQ(text_language_for("es-419").script, std::string("Latn"));
}

TEST_CASE("text language: unknown or mixed scripts are left to the text") {
  // HarfBuzz then takes each text's script from its first letter.
  CHECK(text_language_for("ja").script.empty());
  CHECK(text_language_for("ja-Jpan").script.empty());
  CHECK(text_language_for("ko-Kore").script.empty()); // Hangul and Hanja
  CHECK(text_language_for("tlh").script.empty());
  CHECK_EQ(text_language_for("tlh").tag, std::string("tlh"));
  CHECK(text_language_for("").script.empty());
  CHECK(!text_language_for("tlh").right_to_left);
}
