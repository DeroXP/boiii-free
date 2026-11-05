#include <std_include.hpp>

// xerces_ipc (docs/DESIGN.md 3.1, 15.2): the game role's command pipe, for
// xerces_cli and the launcher. Served only when the launcher started the game
// (XERCES_HOME set) or with -xerces-ipc. Every request runs on the game's main
// thread between frames; a request the main thread cannot start within two
// seconds (a load screen) gets a "busy" error and is dropped, never run late.
//
//   status  build, signatures, plan, journal phase, mods (read only)
//   mods    the main mod and the extra mods, and whether they are loaded
//   reload  {"extras": ["id", ...]} sets the extra mods and reloads them, as
//           the mods menu's ModStack.Set and ModStack.Reload do; front end only
//   unload  no extra mods, then the same reload; front end only

#include <component/mod_stack.hpp>
#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>

#include <xerces_core/handoff.hpp>
#include <xerces_core/ipc.hpp>
#include <xerces_core/journal.hpp>
#include <xerces_core/json.hpp>

#include "runtime.hpp"
#include "signatures.hpp"

namespace xerces_ipc {
namespace {
namespace ipc = xerces_core::ipc;
namespace json = xerces_core::json;

ipc::pipe_server server;

std::string strings(const std::vector<std::string> &items) {
  std::string out = "[";
  for (std::size_t i = 0; i < items.size(); ++i) {
    out += (i ? "," : "") + json::quote(items[i]);
  }
  return out + "]";
}

// Runs fn on the main thread and waits for its result. "busy" is answered
// only for a request the main thread had not started within two seconds,
// which then never runs; one it had started (a reload setting fs_extramods)
// is waited for, so the caller is never told to retry a change that happened.
std::string on_main(const ipc::request &request,
                    std::function<std::string()> fn) {
  auto call = std::make_shared<xerces_core::handoff>(
      [fn = std::move(fn)]() -> std::string {
        try {
          return fn();
        } catch (const std::exception &e) {
          return std::string("!") + e.what();
        } catch (...) {
          return "!the request failed";
        }
      });
  scheduler::once([call] { call->run(); }, scheduler::main);
  std::optional<std::string> result = call->wait(2s);
  if (!result) {
    return ipc::make_error(request.id, ipc::busy,
                           "the game is busy (loading?); try again");
  }
  std::string value = std::move(*result);
  if (!value.empty() && value.front() == '!') {
    return ipc::make_error(request.id, ipc::internal_error, value.substr(1));
  }
  return ipc::make_result(request.id, value);
}

bool in_match() {
  return game::com::Com_IsInGame() && !game::com::Com_IsRunningUILevel();
}

std::string main_mod() {
  const char *id = game::ugc::UGC_ActiveMod_PublisherId();
  return id ? id : "";
}

std::string status_json() {
  const auto journal = xerces_core::parse_journal(xerces_runtime::read_file(
      xerces_runtime::home() / "logs" / "journal.json"));
  char build[16];
  std::snprintf(build, sizeof(build), "0x%X", game::header_checksum());
  const std::string fs_game(game::get_dvar_string("fs_game").value_or(""));
  const std::string extras(game::get_dvar_string("fs_extramods").value_or(""));
  return std::string("{\"role\":\"game\",\"pid\":") +
         std::to_string(GetCurrentProcessId()) +
         ",\"api\":" + std::to_string(ipc::api_version) +
         ",\"build\":" + json::quote(build) + ",\"multi_mod\":" +
         (xerces_signatures::feature_enabled("multi_mod") ? "true" : "false") +
         ",\"plan_id\":" +
         json::quote(journal ? journal->plan_id : std::string()) +
         ",\"phase\":" +
         json::quote(journal ? xerces_core::to_string(journal->phase) : "") +
         ",\"in_match\":" + (in_match() ? "true" : "false") +
         ",\"fs_game\":" + json::quote(fs_game) +
         ",\"fs_extramods\":" + json::quote(extras) +
         ",\"main_mod\":" + json::quote(main_mod()) + "}";
}

std::string mods_json() {
  return "{\"main\":" + json::quote(main_mod()) +
         ",\"extras\":" + strings(mod_stack::get()) +
         ",\"loaded\":" + (mod_stack::is_loaded() ? "true" : "false") + "}";
}

// The extra mods a reload asks for: Workshop ids or folder names, as
// fs_extramods takes them (no commas, at most 37 characters each).
std::optional<std::string> extras_param(const std::string &params,
                                        std::string *error) {
  const auto parsed = json::parse(params);
  if (!parsed.ok || !parsed.document.IsObject()) {
    *error = "params must be an object";
    return std::nullopt;
  }
  const auto *extras = json::member(parsed.document, "extras");
  if (!extras) {
    return std::nullopt; // reload what is set
  }
  if (!extras->IsArray()) {
    *error = "extras must be an array of mod folders";
    return std::nullopt;
  }
  std::string list;
  for (const auto &item : extras->GetArray()) {
    const std::string id =
        item.IsString() ? std::string(item.GetString(), item.GetStringLength())
                        : std::string();
    const bool ok =
        !id.empty() && id.size() <= 37 &&
        std::ranges::all_of(id, [](char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
                 c == ' ';
        });
    if (!ok) {
      *error = "\"" + id + "\" is not a mod folder fs_extramods can take";
      return std::nullopt;
    }
    list += (list.empty() ? "" : ",") + id;
  }
  return list;
}

std::string handle(const ipc::request &request) {
  if (request.method == "status") {
    return on_main(request, status_json);
  }
  if (request.method == "mods") {
    return on_main(request, mods_json);
  }
  if (request.method == "reload" || request.method == "unload") {
    std::string error;
    std::optional<std::string> list;
    if (request.method == "unload") {
      list = "";
    } else {
      list = extras_param(request.params, &error);
      if (!error.empty()) {
        return ipc::make_error(request.id, ipc::invalid_params, error);
      }
    }
    std::string response = on_main(request, [list]() -> std::string {
      if (in_match()) {
        return "!refused";
      }
      if (list) {
        mod_stack::set(*list);
      }
      mod_stack::request_reload();
      xerces_runtime::log_info("ipc.reload", list ? *list : "(unchanged)");
      return mods_json();
    });
    if (response.find("\"message\":\"refused\"") != std::string::npos) {
      return ipc::make_error(request.id, ipc::refused,
                             "the stack changes only in the menus (5.5)");
    }
    return response;
  }
  return ipc::make_error(request.id, ipc::method_not_found,
                         "unknown method " + request.method);
}

bool wanted() {
  int argc = 0;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool flag = false;
  for (int i = 1; argv && i < argc; ++i) {
    if (argv[i][0] == L'+') {
      break;
    }
    flag = flag || _wcsicmp(argv[i], L"-xerces-ipc") == 0;
  }
  if (argv) {
    LocalFree(argv);
  }
  return flag || xerces_runtime::started_by_launcher();
}
} // namespace

class component final : public generic_component {
  DEFINE_COMPONENT_NAME("xerces_ipc");

public:
  void post_unpack() override {
    if (!game::is_client() || !game::is_new_client() || !wanted()) {
      return;
    }
    std::string error;
    if (server.start(xerces_runtime::home() / "run", "game", handle, &error)) {
      xerces_runtime::log_info("ipc.start", server.pipe_name());
    } else {
      xerces_runtime::log_error("ipc.failed", error);
    }
  }

  void pre_destroy() override { server.stop(); }
};
} // namespace xerces_ipc

REGISTER_COMPONENT(xerces_ipc::component)
