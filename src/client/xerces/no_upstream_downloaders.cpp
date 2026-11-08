#include <std_include.hpp>

// Stand-ins for the functions of upstream's downloaders that other code calls,
// for builds without them (D15, xerces/switches.hpp). The downloaders' own
// sources are untouched and simply not compiled; premake leaves out
// workshop/steamcmd.cpp, workshop/steam_cdn.cpp, workshop/download.cpp,
// fastdl.cpp and launcher/launcher_workshop.cpp.
#ifndef XERCES_WITH_UPSTREAM_DOWNLOADERS

#include "component/download_overlay.hpp"
#include "component/fastdl.hpp"
#include "component/scheduler.hpp"
#include "component/workshop/workshop.hpp"
#include "launcher/launcher_workshop.hpp"

#include <game/game.hpp>

#include <xerces/runtime.hpp>
#include <xerces_core/workshop_get.hpp>

// Xerces (DESIGN.md 9.2): the in-game Workshop menu's downloads go through
// Steam itself. Each item in the queue is subscribed to and downloaded by
// Steam's own Workshop API under the player's Steam account, in
// `xerces_cli workshop get` (xerces_core/workshop_get.hpp), a process of its
// own next to this program; Steam puts it in its Workshop folder and the game
// reloads its content. None of upstream's downloaders comes back with this.
namespace {
void popup(const std::string &text) {
  scheduler::once(
      [text] {
        game::ui::UI_OpenErrorPopupWithMessage(
            game::LOCAL_CLIENT_0, game::errorCode::UI, text.c_str());
      },
      scheduler::main);
}

std::filesystem::path program_dir() {
  std::wstring module(32768, L'\0');
  const DWORD size = GetModuleFileNameW(nullptr, module.data(),
                                        static_cast<DWORD>(module.size()));
  module.resize(size);
  return std::filesystem::path(module).parent_path();
}

std::mutex queue_mutex;
std::vector<workshop::queue::entry> entries; // queued, the current one, then
                                             // those done or failed
std::uint32_t queue_generation = 1;
std::string current_item;
bool worker_running = false;
std::atomic<void *> current_process{nullptr};

// Call with queue_mutex held.
void changed() { ++queue_generation; }

void finish_current(workshop::queue::item_state state) {
  std::lock_guard lock(queue_mutex);
  for (auto &entry : entries) {
    if (entry.id == current_item) {
      entry.state = state;
    }
  }
  current_item.clear();
  changed();
}

// One item at a time, until the queue is empty.
void run_queue() {
  for (;;) {
    workshop::queue::entry next;
    {
      std::lock_guard lock(queue_mutex);
      const auto it = std::ranges::find_if(entries, [](const auto &entry) {
        return entry.state == workshop::queue::item_state::queued;
      });
      if (it == entries.end()) {
        worker_running = false;
        download_overlay::clear();
        return;
      }
      it->state = workshop::queue::item_state::downloading;
      current_item = it->id;
      next = *it;
      changed();
    }
    std::uint64_t id = 0;
    std::from_chars(next.id.data(), next.id.data() + next.id.size(), id);
    const std::string name = next.title.empty() ? next.id : next.title;
    download_overlay::download_state ui{};
    ui.active = true;
    ui.hidden = !next.show_overlay;
    ui.item_name = name;
    ui.status_line = "Steam: subscribing";
    ui.on_cancel = [] {
      if (void *process = current_process.load()) {
        TerminateProcess(static_cast<HANDLE>(process), 3);
      }
    };
    download_overlay::update(ui);

    std::string failure;
    bool installed = false;
    std::string error;
    const int code = xerces_core::workshop_get::run_helper(
        program_dir() / "xerces_cli.exe", game::get_game_path(), {id},
        [&](const xerces_core::workshop_get::progress &line) {
          if (line.state == "error" || line.state == "failed") {
            failure = line.message;
            return;
          }
          if (line.state == "installed") {
            installed = true;
          }
          ui.downloaded_bytes = static_cast<std::size_t>(line.done);
          ui.total_bytes = static_cast<std::size_t>(line.total);
          ui.status_line = "Steam: " + line.state;
          download_overlay::update(ui);
        },
        &current_process, &error);
    if (code < 0) {
      failure = error;
    }
    if (installed) {
      printf("[ Workshop ] Steam installed %s\n", next.id.c_str());
      xerces_runtime::log_info("workshop.steam.installed",
                               "Steam downloaded a Workshop item");
      finish_current(workshop::queue::item_state::done);
      workshop::request_content_reload(name);
    } else {
      xerces_runtime::log_warn("workshop.steam.failed",
                               failure.empty() ? "stopped" : failure);
      finish_current(failure.empty() ? workshop::queue::item_state::cancelled
                                     : workshop::queue::item_state::failed);
      if (!failure.empty()) {
        popup("Steam did not download " + name + ": " + failure);
      }
    }
  }
}
} // namespace

namespace workshop {
void start_download(const std::string_view &id, game::ZoneType type) {
  queue::add(id, type);
}

bool cancel_download() {
  if (void *process = current_process.load()) {
    TerminateProcess(static_cast<HANDLE>(process), 3);
    return true;
  }
  return false;
}

// The in-game Workshop UI's download queue, worked through by Steam.
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
         const std::string_view &title, std::uint64_t file_size,
         bool show_overlay) {
  if (!xerces_core::workshop_get::parse_ids(id)) {
    return false;
  }
  std::lock_guard lock(queue_mutex);
  const auto it = std::ranges::find(entries, id, &entry::id);
  if (it != entries.end()) {
    if (it->state == item_state::queued ||
        it->state == item_state::downloading) {
      return true;
    }
    entries.erase(it); // done or failed before: again
  }
  entry added;
  added.id = std::string(id);
  added.type = type;
  added.title = std::string(title);
  added.file_size = file_size;
  added.show_overlay = show_overlay;
  entries.push_back(std::move(added));
  changed();
  if (!worker_running) {
    worker_running = true;
    std::thread(run_queue).detach();
  }
  return true;
}

bool remove(const std::string_view &id) {
  std::lock_guard lock(queue_mutex);
  const auto it = std::ranges::find(entries, id, &entry::id);
  if (it == entries.end() || it->state == item_state::downloading) {
    return false;
  }
  entries.erase(it);
  changed();
  return true;
}

bool move(const std::string_view &, int) { return false; }

void clear() {
  std::lock_guard lock(queue_mutex);
  std::erase_if(entries, [](const entry &item) {
    return item.state != item_state::downloading;
  });
  changed();
}

std::vector<entry> list() {
  std::lock_guard lock(queue_mutex);
  std::vector<entry> out = entries;
  int place = 0;
  for (auto &item : out) {
    item.position = item.state == item_state::queued ||
                            item.state == item_state::downloading
                        ? ++place
                        : -1;
  }
  return out;
}

int position(const std::string_view &id) {
  for (const auto &item : list()) {
    if (item.id == id) {
      return item.position;
    }
  }
  return -1;
}

std::string current_id() {
  std::lock_guard lock(queue_mutex);
  return current_item;
}

std::uint32_t generation() {
  std::lock_guard lock(queue_mutex);
  return queue_generation;
}
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
