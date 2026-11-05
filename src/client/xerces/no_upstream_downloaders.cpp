#include <std_include.hpp>

// Stand-ins for the functions of upstream's downloaders that other code calls,
// for builds without them (D15, xerces/switches.hpp). The downloaders' own
// sources are untouched and simply not compiled; premake leaves out
// workshop/steamcmd.cpp, workshop/steam_cdn.cpp, workshop/download.cpp,
// fastdl.cpp and launcher/launcher_workshop.cpp.
#ifndef XERCES_WITH_UPSTREAM_DOWNLOADERS

#include "component/fastdl.hpp"
#include "component/scheduler.hpp"
#include "component/workshop/workshop.hpp"
#include "launcher/launcher_workshop.hpp"

#include <game/game.hpp>

namespace {
void explain_no_downloads() {
  scheduler::once(
      [] {
        game::ui::UI_OpenErrorPopupWithMessage(
            game::LOCAL_CLIENT_0, game::errorCode::UI,
            "This build does not download Workshop items itself. Subscribe to "
            "the item on the Steam Workshop; Steam downloads it and the game "
            "finds it.");
      },
      scheduler::main);
}
} // namespace

namespace workshop {
void start_download(const std::string_view &id, game::ZoneType) {
  printf("[ Workshop ] Not downloading %.*s: in-game downloads are not part "
         "of this build (D15). Subscribe in Steam.\n",
         static_cast<int>(id.size()), id.data());
  explain_no_downloads();
}

bool cancel_download() { return false; }

// The in-game Workshop UI's download queue: always empty here, and adding to
// it explains where Workshop items come from instead.
namespace queue {
const char *state_name(const item_state state) {
  switch (state) {
  case item_state::queued:
    return "queued";
  case item_state::downloading:
    return "downloading";
  case item_state::done:
    return "done";
  case item_state::failed:
    return "failed";
  default:
    return "cancelled";
  }
}

bool add(const std::string_view &id, game::ZoneType type,
         const std::string_view &, std::uint64_t, bool) {
  start_download(id, type);
  return false;
}

bool remove(const std::string_view &) { return false; }
bool move(const std::string_view &, int) { return false; }
void clear() {}
std::vector<entry> list() { return {}; }
int position(const std::string_view &) { return -1; }
std::string current_id() { return {}; }
std::uint32_t generation() { return 0; }
} // namespace queue
} // namespace workshop

namespace fastdl {
bool start_download(const std::shared_ptr<download_context> context) {
  if (context && context->failure_callback) {
    context->failure_callback();
  }
  return false;
}

void cancel_download() {}

bool is_downloading() { return false; }
} // namespace fastdl

namespace launcher::workshop {
CComVariant utf8_variant(const std::string &utf8_str) {
  if (utf8_str.empty()) {
    return CComVariant(L"");
  }
  const int wide_len =
      MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(),
                          static_cast<int>(utf8_str.size()), nullptr, 0);
  if (wide_len <= 0) {
    return CComVariant(utf8_str.c_str());
  }
  std::wstring wide(static_cast<size_t>(wide_len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(),
                      static_cast<int>(utf8_str.size()), wide.data(), wide_len);
  return CComVariant(wide.c_str());
}

void register_callbacks(html_frame *) {}

// Workshop metadata for the old launcher's Workshop tab (scraped from Steam's
// web pages): none without the downloaders.
std::map<std::string, workshop_item_meta>
batch_get_workshop_meta(const std::vector<std::string> &) {
  return {};
}

void try_refresh_workshop_content() {
  try {
    game::cbuf::Cbuf_AddText(game::LOCAL_CLIENT_0, "userContentReload\n");
  } catch (...) {
    // The game is not running yet; nothing to refresh.
  }
}
} // namespace launcher::workshop

#endif
