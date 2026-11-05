#include "settings_options.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "json.hpp"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// wingdi.h's GetObject macro would rename rapidjson's GetObject().
#undef GetObject

namespace xerces_core {
namespace {
namespace fs = std::filesystem;

constexpr std::string_view name_key = "playerName";

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view trim(std::string_view text) {
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

// The client writes the file without a byte order mark and reads it with or
// without one; Xerces' parser wants none.
std::string_view without_bom(std::string_view text) {
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF) {
    text.remove_prefix(3);
  }
  return text;
}

std::optional<std::string> read_all(const fs::path &file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

std::string bare(std::string_view flag) {
  while (!flag.empty() && flag.front() == '-') {
    flag.remove_prefix(1);
  }
  return std::string(flag);
}

// Holds the client's properties lock (utils::properties::lock(), a named
// mutex in the session namespace) for as long as it lives.
class properties_lock {
public:
  properties_lock() {
    handle_ = CreateMutexA(nullptr, FALSE, "boiii-properties-lock");
    if (handle_ == nullptr) {
      return;
    }
    const DWORD waited = WaitForSingleObject(handle_, 3000);
    owned_ = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
  }
  ~properties_lock() {
    if (handle_ == nullptr) {
      return;
    }
    if (owned_) {
      ReleaseMutex(handle_);
    }
    CloseHandle(handle_);
  }
  properties_lock(const properties_lock &) = delete;
  properties_lock &operator=(const properties_lock &) = delete;
  // A failed CreateMutex still lets the write go ahead: the lock only keeps
  // a running client from reading a half-written file, and the write goes
  // through a temporary file anyway.
  bool usable() const { return handle_ == nullptr || owned_; }

private:
  HANDLE handle_ = nullptr;
  bool owned_ = false;
};
} // namespace

// ------------------------------------------------------------- player name
player_name_result clean_player_name(std::string_view typed) {
  player_name_result out;
  std::string kept;
  for (const char c : typed) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte >= 32 && byte <= 126) {
      kept.push_back(c);
    } else if (!(c == '\t' || c == '\r' || c == '\n')) {
      out.dropped = true; // whitespace control characters are just trimmed
    }
  }
  std::string_view name = trim(kept);
  if (name.size() > player_name_max) {
    name = trim(name.substr(0, player_name_max));
    out.shortened = true;
  }
  out.name = std::string(name);
  return out;
}

fs::path player_properties_file(const fs::path &game_dir) {
  return game_dir / "boiii_players" / "properties.json";
}

std::optional<std::string> read_player_name(std::string_view properties_text) {
  const std::string_view text = trim(without_bom(properties_text));
  if (text.empty()) {
    return std::nullopt;
  }
  const json::parse_result parsed = json::parse(text);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return std::nullopt;
  }
  return json::get_string(parsed.document, name_key);
}

properties_edit with_player_name(std::string_view properties_text,
                                 std::string_view name) {
  properties_edit out;
  const std::string_view text = trim(without_bom(properties_text));
  rapidjson::Document doc;
  if (text.empty()) {
    doc.SetObject(); // the client reads an empty file as no properties
  } else {
    json::parse_result parsed = json::parse(text);
    if (!parsed.ok) {
      out.error = "properties.json is not valid JSON (line " +
                  std::to_string(parsed.line) + "): " + parsed.error;
      out.failure = name_failure::not_json;
      return out;
    }
    if (!parsed.document.IsObject()) {
      out.error = "properties.json does not hold a JSON object.";
      out.failure = name_failure::not_object;
      return out;
    }
    doc.Swap(parsed.document);
  }
  // The client removes every copy of a key before it adds it again.
  const rapidjson::Value key(rapidjson::StringRef(name_key.data(),
                                                  static_cast<rapidjson::SizeType>(
                                                      name_key.size())));
  while (doc.HasMember(key)) {
    doc.RemoveMember(key);
  }
  if (!name.empty()) {
    auto &alloc = doc.GetAllocator();
    doc.AddMember(rapidjson::Value(name_key.data(),
                                   static_cast<rapidjson::SizeType>(name_key.size()),
                                   alloc),
                  rapidjson::Value(name.data(),
                                   static_cast<rapidjson::SizeType>(name.size()),
                                   alloc),
                  alloc);
  }
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  out.text.assign(buffer.GetString(), buffer.GetSize());
  out.ok = true;
  return out;
}

name_apply_result apply_player_name(const fs::path &game_dir,
                                    std::string_view name) {
  name_apply_result out;
  const fs::path file = player_properties_file(game_dir);
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) {
    out.status = name_apply::no_file;
    out.message = "The game folder has no boiii_players\\properties.json yet.";
    return out;
  }
  const properties_lock lock;
  if (!lock.usable()) {
    out.failure = name_failure::busy;
    out.message = "The game is busy with its properties file; try again.";
    return out;
  }
  const std::optional<std::string> text = read_all(file);
  if (!text) {
    out.failure = name_failure::unreadable;
    out.message = "boiii_players\\properties.json could not be read.";
    return out;
  }
  const std::optional<std::string> current = read_player_name(*text);
  if (name.empty() ? !current.has_value() : (current && *current == name)) {
    out.status = name_apply::unchanged;
    return out;
  }
  const properties_edit edit = with_player_name(*text, name);
  if (!edit.ok) {
    out.failure = edit.failure;
    out.message = edit.error;
    return out;
  }
  // Through a temporary file, so the client never sees half a file.
  fs::path temporary = file;
  temporary += ".xerces-tmp";
  {
    std::ofstream write(temporary, std::ios::binary | std::ios::trunc);
    write.write(edit.text.data(), static_cast<std::streamsize>(edit.text.size()));
    // The text sits in the stream's buffer until it closes, and a full disk
    // only shows then: check after close, or a short file would replace the
    // game's and lose its other settings.
    write.close();
    if (!write) {
      out.failure = name_failure::write_failed;
      out.message = "boiii_players\\properties.json could not be written.";
      fs::remove(temporary, ec);
      return out;
    }
  }
  fs::rename(temporary, file, ec);
  if (ec) {
    out.failure = name_failure::replace_failed;
    out.message = "boiii_players\\properties.json could not be replaced: " +
                  ec.message();
    fs::remove(temporary, ec);
    return out;
  }
  out.status = name_apply::written;
  return out;
}

bool game_name_differs(std::string_view saved,
                       const std::optional<std::string> &in_file) {
  if (saved.empty()) {
    // The client reads an empty playerName as none (name.cpp).
    return in_file.has_value() && !in_file->empty();
  }
  return in_file.value_or("") != saved;
}

pending_name_step pending_name_after(bool folder_changed, bool file_exists) {
  if (!file_exists) {
    return pending_name_step::wait;
  }
  return folder_changed ? pending_name_step::write : pending_name_step::ask;
}

// ------------------------------------------------------------ launch flags
bool valid_launch_flag(std::string_view flag) {
  if (flag.size() < 2 || flag.size() > 64 || flag[0] != '-') {
    return false;
  }
  return std::ranges::all_of(flag.substr(1), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
}

flags_parse parse_launch_flags(std::string_view text) {
  flags_parse out;
  std::size_t at = 0;
  while (at < text.size()) {
    while (at < text.size() && is_space(text[at])) {
      ++at;
    }
    const std::size_t start = at;
    while (at < text.size() && !is_space(text[at])) {
      ++at;
    }
    if (start == at) {
      break;
    }
    const std::string token(text.substr(start, at - start));
    if (!valid_launch_flag(token)) {
      out.invalid.push_back(token);
      continue;
    }
    const std::string name = bare(token);
    if (name == "launch" || name == "xerces-plan") {
      out.reserved.push_back(token);
      continue;
    }
    if (std::ranges::find(out.flags, token) != out.flags.end()) {
      continue;
    }
    if (out.flags.size() == max_launch_flags) {
      out.too_many = true;
      continue;
    }
    out.flags.push_back(token);
  }
  return out;
}

std::string join_flags(const std::vector<std::string> &flags) {
  std::string out;
  for (const std::string &flag : flags) {
    if (!out.empty()) {
      out.push_back(' ');
    }
    out += flag;
  }
  return out;
}

namespace {
// A spelling the client's parser takes for the flag name: "-name",
// "--name", or one of its short forms (src/common/utils/flags.cpp registers
// "-nc", "-nocinematics" and "--nocinematics" as one argument). Add a flag's
// short forms here when a switch on the screen manages it.
bool spells(std::string_view flag, std::string_view name) {
  if ((flag.starts_with("-") && flag.substr(1) == name) ||
      (flag.starts_with("--") && flag.substr(2) == name)) {
    return true;
  }
  return name == "nocinematics" && flag == "-nc";
}
} // namespace

bool has_flag(const std::vector<std::string> &flags, std::string_view name) {
  return std::ranges::any_of(
      flags, [&](const std::string &flag) { return spells(flag, name); });
}

std::vector<std::string> with_flag(std::vector<std::string> flags,
                                   std::string_view name, bool on) {
  std::erase_if(flags,
                [&](const std::string &flag) { return spells(flag, name); });
  if (on) {
    flags.push_back("-" + std::string(name));
  }
  return flags;
}

std::string display_mode(const std::vector<std::string> &flags) {
  if (has_flag(flags, "borderless")) {
    return "borderless";
  }
  if (has_flag(flags, "windowed")) {
    return "windowed";
  }
  return "game";
}

std::vector<std::string> with_display_mode(std::vector<std::string> flags,
                                           std::string_view mode) {
  flags = with_flag(std::move(flags), "borderless", false);
  flags = with_flag(std::move(flags), "windowed", false);
  if (mode == "borderless" || mode == "windowed") {
    flags = with_flag(std::move(flags), mode, true);
  }
  return flags;
}

fs::path game_config_file(const fs::path &game_dir) {
  return game_dir / "boiii_players" / "user" / "config.cfg";
}

namespace {
bool same_letters(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::ranges::equal(a, b, [](char x, char y) {
           const auto lower = [](char c) {
             return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
           };
           return lower(x) == lower(y);
         });
}

// The next word of a config line: a "quoted" run (without its quotes) or a
// run up to the next space.
std::string_view next_word(std::string_view &line) {
  while (!line.empty() && is_space(line.front())) {
    line.remove_prefix(1);
  }
  if (line.empty()) {
    return {};
  }
  if (line.front() == '"') {
    line.remove_prefix(1);
    const std::size_t end = line.find('"');
    const std::string_view word = line.substr(0, end);
    line.remove_prefix(end == std::string_view::npos ? line.size() : end + 1);
    return word;
  }
  std::size_t end = 0;
  while (end < line.size() && !is_space(line[end])) {
    ++end;
  }
  const std::string_view word = line.substr(0, end);
  line.remove_prefix(end);
  return word;
}
} // namespace

std::optional<std::string> read_saved_dvar(std::string_view config_text,
                                           std::string_view name) {
  std::optional<std::string> found;
  while (!config_text.empty()) {
    const std::size_t end = config_text.find('\n');
    std::string_view line = config_text.substr(0, end);
    config_text.remove_prefix(end == std::string_view::npos ? config_text.size()
                                                            : end + 1);
    const std::string_view command = next_word(line);
    if (!same_letters(command, "set") && !same_letters(command, "seta") &&
        !same_letters(command, "sets")) {
      continue;
    }
    if (!same_letters(next_word(line), name) || trim(line).empty()) {
      continue; // another dvar, or "set name" alone, which changes nothing
    }
    found = std::string(trim(next_word(line)));
  }
  return found;
}

std::optional<bool> saved_dvar_on(std::string_view config_text,
                                  std::string_view name) {
  const std::optional<std::string> value = read_saved_dvar(config_text, name);
  if (!value) {
    return std::nullopt;
  }
  return !(value->empty() || *value == "0" || same_letters(*value, "false"));
}

// -------------------------------------------------------------- game options
number_input parse_number_input(std::string_view text, int min, int max) {
  number_input out;
  text = trim(text);
  if (text.empty()) {
    return out;
  }
  out.state = number_input::kind::invalid;
  if (text.size() > 7 || !std::ranges::all_of(text, [](char c) {
        return c >= '0' && c <= '9';
      })) {
    return out;
  }
  const int value = std::atoi(std::string(text).c_str());
  if (value < min || value > max) {
    return out;
  }
  out.state = number_input::kind::ok;
  out.value = value;
  return out;
}

const std::vector<int> &fps_limit_stops() {
  static const std::vector<int> stops = {30,  60,  75,  90,  100, 120, 144,
                                         165, 180, 200, 240, 300, 360, 500};
  return stops;
}

int fps_slider_max() { return static_cast<int>(fps_limit_stops().size()) + 1; }

int fps_slider_index(std::optional<int> limit) {
  if (!limit) {
    return 0;
  }
  if (*limit <= 0) {
    return fps_slider_max();
  }
  const std::vector<int> &stops = fps_limit_stops();
  std::size_t best = 0;
  for (std::size_t i = 1; i < stops.size(); ++i) {
    if (std::abs(stops[i] - *limit) < std::abs(stops[best] - *limit)) {
      best = i;
    }
  }
  return static_cast<int>(best) + 1;
}

std::optional<int> fps_slider_value(int index) {
  if (index <= 0) {
    return std::nullopt;
  }
  if (index >= fps_slider_max()) {
    return 0;
  }
  return fps_limit_stops()[static_cast<std::size_t>(index - 1)];
}
} // namespace xerces_core
