// Checks that the launcher's HarfBuzz font engine never draws a line wider
// than RmlUi lays it out (docs/DESIGN.md 7.1, docs/TESTING.md). RmlUi 6.3's
// ElementText lays a line out as the sum of its words, each with the space
// before it, measured one by one with GetStringWidth, and draws the whole line
// with GenerateString; a line drawn wider than its box can get an ellipsis it
// does not need. This drives the built engine (xerces_rmlui_harfbuzz) the same
// way for every string, weight and size it is given. Not part of Xerces.exe:
// tools\dev\launcher\font-width-check.ps1 builds and runs it.
//
//   font-width-check <fonts dir> <data\xerces\i18n\template.json>
//
// The strings are the template's keys, the launcher's English text. Exit code
// 0 when every line fits, 1 when one does not, 2 on a setup error.

#include <RmlUi/Core.h>
#include <RmlUi/Core/RenderManager.h>

// RmlUi's sample header (Samples/basic/harfbuzz/src).
#include <FontEngineInterfaceHarfBuzz.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {
// One JSON string at text[at] (the opening quote), decoded to UTF-8; at moves
// past the closing quote. Enough for template.json, which
// tools\i18n\extract_strings.py writes.
bool read_json_string(const std::string &text, size_t &at, std::string &out) {
  out.clear();
  if (at >= text.size() || text[at] != '"') {
    return false;
  }
  for (++at; at < text.size(); ++at) {
    const char c = text[at];
    if (c == '"') {
      ++at;
      return true;
    }
    if (c != '\\') {
      out += c;
      continue;
    }
    if (++at >= text.size()) {
      return false;
    }
    switch (text[at]) {
    case 'n':
      out += '\n';
      break;
    case 't':
      out += '\t';
      break;
    case 'r':
      out += '\r';
      break;
    case 'b':
      out += '\b';
      break;
    case 'f':
      out += '\f';
      break;
    case 'u': {
      if (at + 4 >= text.size()) {
        return false;
      }
      unsigned code = std::stoul(text.substr(at + 1, 4), nullptr, 16);
      at += 4;
      // A surrogate pair is two \u escapes.
      if (code >= 0xD800 && code < 0xDC00 && at + 6 < text.size() &&
          text[at + 1] == '\\' && text[at + 2] == 'u') {
        const unsigned low = std::stoul(text.substr(at + 3, 4), nullptr, 16);
        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        at += 6;
      }
      Rml::String utf8 = Rml::StringUtilities::ToUTF8(Rml::Character(code));
      out += utf8;
      break;
    }
    default: // " \ /
      out += text[at];
    }
  }
  return false;
}

// The keys of a JSON object of strings.
bool read_template_keys(const std::string &path,
                        std::vector<std::string> &keys) {
  std::ifstream in(path, std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
  size_t at = text.find('{');
  if (at == std::string::npos) {
    return false;
  }
  ++at;
  std::string key, value;
  while (true) {
    at = text.find_first_not_of(" \t\r\n,", at);
    if (at == std::string::npos) {
      return false;
    }
    if (text[at] == '}') {
      return true;
    }
    if (!read_json_string(text, at, key)) {
      return false;
    }
    at = text.find_first_not_of(" \t\r\n:", at);
    if (at == std::string::npos || !read_json_string(text, at, value)) {
      return false;
    }
    if (key.empty() || key.starts_with('_')) {
      continue;
    }
    // White space collapses to single spaces in the launcher's documents.
    std::replace_if(
        key.begin(), key.end(),
        [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
    keys.push_back(key);
  }
}

// Text geometry is made but never drawn.
class null_render_interface : public Rml::RenderInterface {
public:
  Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>,
                                              Rml::Span<const int>) override {
    return 1;
  }
  void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f,
                      Rml::TextureHandle) override {}
  void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
  Rml::TextureHandle LoadTexture(Rml::Vector2i &,
                                 const Rml::String &) override {
    return 0;
  }
  Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>,
                                     Rml::Vector2i) override {
    return 1;
  }
  void ReleaseTexture(Rml::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(Rml::Rectanglei) override {}
};

// The words of a line as RmlUi measures them with white space collapsed:
// "word", then " word" for each next word.
std::vector<std::string> words(const std::string &line) {
  std::vector<std::string> out;
  size_t begin = 0;
  while (begin < line.size()) {
    size_t end = begin;
    while (end < line.size() && line[end] == ' ') {
      ++end;
    }
    while (end < line.size() && line[end] != ' ') {
      ++end;
    }
    out.push_back(line.substr(begin, end - begin));
    begin = end;
  }
  return out;
}

struct face {
  const char *family; // lower case, as RmlUi's font provider keys families
  int weight;
};
} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: font-width-check <fonts dir> <template.json>\n");
    return 2;
  }
  const std::string fonts = argv[1];
  std::vector<std::string> lines;
  if (!read_template_keys(argv[2], lines) || lines.empty()) {
    std::fprintf(stderr, "no strings read from %s\n", argv[2]);
    return 2;
  }

  static null_render_interface render;
  static FontEngineInterfaceHarfBuzz engine;
  Rml::SetRenderInterface(&render);
  Rml::SetFontEngineInterface(&engine);
  Rml::Initialise();
  // As the launcher registers English (src/xerces_launcher/font_engine.cpp).
  engine.RegisterLanguage("en", "Latn", TextFlowDirection::LeftToRight);
  for (const char *file : {"InterTight-wght.ttf", "IBMPlexMono-Regular.ttf",
                           "IBMPlexMono-Medium.ttf"}) {
    if (!Rml::LoadFontFace(fonts + "/" + file)) {
      std::fprintf(stderr, "could not load %s\n", file);
      Rml::Shutdown();
      return 2;
    }
  }

  int result = 0;
  // The engine's glyph textures belong to this render manager, so it outlives
  // Rml::Shutdown, which releases them.
  auto render_manager_owner = std::make_unique<Rml::RenderManager>(&render);
  Rml::RenderManager &render_manager = *render_manager_owner;
  {
    const Rml::TextShapingContext context{"en", Rml::Style::Direction::Auto,
                                          Rml::Style::FontKerning::Auto, 0.f};
    const face faces[] = {{"inter tight", 400},   {"inter tight", 500},
                          {"inter tight", 600},   {"inter tight", 700},
                          {"inter tight", 800},   {"inter tight", 900},
                          {"ibm plex mono", 400}, {"ibm plex mono", 500}};
    long measured = 0, wider = 0, narrower = 0, worst = 0;
    for (const face &f : faces) {
      for (int size :
           {12, 13, 14, 16, 18, 20, 21, 24, 28, 30, 40, 42, 60, 80, 96, 120}) {
        const Rml::FontFaceHandle handle =
            engine.GetFontFaceHandle(f.family, Rml::Style::FontStyle::Normal,
                                     Rml::Style::FontWeight(f.weight), size);
        if (!handle) {
          std::fprintf(stderr, "no face %s %d\n", f.family, f.weight);
          result = 2;
          continue;
        }
        for (const std::string &line : lines) {
          int laid_out = 0;
          for (const std::string &word : words(line)) {
            laid_out += engine.GetStringWidth(handle, word, context,
                                              Rml::Character::Null);
          }
          Rml::TexturedMeshList meshes;
          const int drawn = engine.GenerateString(
              render_manager, handle, 0, line, Rml::Vector2f(0, 0),
              Rml::ColourbPremultiplied(255), 1.f, context, meshes);
          const int measured_whole = engine.GetStringWidth(
              handle, line, context, Rml::Character::Null);
          ++measured;
          if (drawn > laid_out || measured_whole != drawn) {
            ++wider;
            worst = std::max<long>(worst, drawn - laid_out);
            if (wider <= 10) {
              std::printf("%s %d, %d px: drawn %d, laid out %d, measured "
                          "whole %d: %.70s\n",
                          f.family, f.weight, size, drawn, laid_out,
                          measured_whole, line.c_str());
            }
          } else if (drawn < laid_out) {
            ++narrower;
          }
        }
      }
    }
    std::printf("%ld lines: %ld drawn wider than laid out or measured (worst "
                "%ld px), %ld narrower\n",
                measured, wider, worst, narrower);
    std::fflush(stdout);
    if (wider > 0 && result == 0) {
      result = 1;
    }
  }
  Rml::Shutdown();
  render_manager_owner.reset();
  return result;
}
