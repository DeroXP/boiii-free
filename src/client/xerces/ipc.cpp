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
//   host    whether others can join this game by address now, its UDP port
//           and the router's outside address while UPnP forwards the port
//           (friends' invites, docs/DESIGN.md 7.3)
//   join    {"address": "host:port"} joins that lobby as "connect" in the
//           console does; at the main menu only (an invite accepted in
//           Xerces while the game runs)
//   notify  {"title", "text"} shows a toast (an invite came in)

#include <component/gpu.hpp>
#include <component/mod_stack.hpp>
#include <component/nat.hpp>
#include <component/scheduler.hpp>
#include <component/toast.hpp>
#include <game/game.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>

#include <xerces_core/handoff.hpp>
#include <xerces_core/ipc.hpp>
#include <xerces_core/journal.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/launch.hpp>

#include "join_password.hpp"
#include "perf.hpp"
#include "punch.hpp"
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
//
// Once the game is quitting, its main thread is in the exit (main.cpp's exit
// hook, which stops this pipe and joins its thread) and takes nothing more.
// The wait ends there at once: on 2026-11-12 a status request from a
// reopened launcher kept the exit waiting for these two seconds, and in them
// a game thread opened a file that the game's runtime had already closed its
// table for, which was reported as a crash of a game that had quit.
std::string on_main(const ipc::request &request,
                    std::function<std::string()> fn) {
  const auto quitting = [] { return xerces_runtime::exit_code().has_value(); };
  if (quitting()) {
    return ipc::make_error(request.id, ipc::busy, "the game is closing");
  }
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
  std::optional<std::string> result = call->wait(2s, quitting);
  if (!result) {
    return ipc::make_error(request.id, ipc::busy,
                           quitting()
                               ? "the game is closing"
                               : "the game is busy (loading?); try again");
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
        !id.empty() && id.size() <= 37 && std::ranges::all_of(id, [](char c) {
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

std::string host_json() {
  const nat::hosting_state state = nat::hosting();
  return std::string("{\"in_match\":") + (in_match() ? "true" : "false") +
         ",\"invite_only\":" +
         (xerces_join_password::invite_only() ? "true" : "false") +
         ",\"main_menu\":" + (gpu::on_main_menu() ? "true" : "false") +
         ",\"mapped\":" + json::quote(state.mapped_endpoint) +
         ",\"open\":" + (state.open ? "true" : "false") +
         ",\"port\":" + std::to_string(state.port) + "}";
}

// A string member of the params object, at most max_bytes and without
// control characters; nullopt (with error) otherwise.
std::optional<std::string> text_param(const std::string &params,
                                      std::string_view name,
                                      std::size_t max_bytes,
                                      std::string *error) {
  const auto parsed = json::parse(params);
  const auto value = parsed.ok && parsed.document.IsObject()
                         ? json::get_string(parsed.document, name)
                         : std::nullopt;
  if (!value || value->empty() || value->size() > max_bytes ||
      std::ranges::any_of(*value, [](char c) {
        return static_cast<unsigned char>(c) < 0x20 || c == 0x7f;
      })) {
    *error = std::string(name) + " must be a short line of text";
    return std::nullopt;
  }
  return value;
}

std::string handle(const ipc::request &request) {
  if (request.method == "status") {
    return on_main(request, status_json);
  }
  if (request.method == "host") {
    return on_main(request, host_json);
  }
  if (request.method == "join") {
    std::string error;
    const auto address = text_param(request.params, "address", 64, &error);
    if (!address || !xerces_core::valid_server_address(*address)) {
      return ipc::make_error(request.id, ipc::invalid_params,
                             error.empty() ? "address is not host:port"
                                           : error);
    }
    // The invite's ticket for this host (join_password.hpp), if it has one.
    std::string ticket_error;
    const auto ticket = text_param(request.params, "ticket", 64, &ticket_error);
    std::string response = on_main(request, [address, ticket]() -> std::string {
      // As xerces/join_after_menu.cpp: a join before the main menu sends
      // stats the game has not loaded yet.
      if (in_match() || !gpu::on_main_menu()) {
        return "!refused";
      }
      if (ticket) {
        xerces_join_password::set_join_ticket(*address, *ticket);
      }
      xerces_runtime::log_info("xerces.join.invite",
                               "joining the lobby of an invite");
      const std::string command = "connect " + *address + "\n";
      game::cbuf::Cbuf_AddText(game::LOCAL_CLIENT_0, command.c_str());
      return "{\"joining\":true}";
    });
    if (response.find("\"message\":\"refused\"") != std::string::npos) {
      return ipc::make_error(request.id, ipc::refused,
                             "the game joins from its main menu only");
    }
    return response;
  }
  if (request.method == "notify") {
    std::string error;
    const auto title = text_param(request.params, "title", 80, &error);
    const auto text =
        title ? text_param(request.params, "text", 200, &error) : std::nullopt;
    if (!title || !text) {
      return ipc::make_error(request.id, ipc::invalid_params, error);
    }
    toast::info(*title, *text);
    return ipc::make_result(request.id, "{\"shown\":true}");
  }
  if (request.method == "mods") {
    return on_main(request, mods_json);
  }
  if (request.method == "guests") {
    // Who may join this game's lobby (join_password.hpp): invite_only, and
    // a ticket for an invite just sent, good for `seconds`.
    const auto parsed = json::parse(request.params);
    if (!parsed.ok || !parsed.document.IsObject()) {
      return ipc::make_error(request.id, ipc::invalid_params,
                             "params must be an object");
    }
    const auto &params = parsed.document;
    if (params.HasMember("invite_only")) {
      if (!params["invite_only"].IsBool()) {
        return ipc::make_error(request.id, ipc::invalid_params,
                               "invite_only must be true or false");
      }
      xerces_join_password::set_invite_only(params["invite_only"].GetBool());
    }
    if (params.HasMember("ticket")) {
      const auto ticket = json::get_string(params, "ticket");
      const int seconds =
          params.HasMember("seconds") && params["seconds"].IsInt()
              ? params["seconds"].GetInt()
              : 0;
      if (!ticket || !xerces_join_password::add_ticket(
                         *ticket, std::chrono::seconds(seconds))) {
        return ipc::make_error(request.id, ipc::invalid_params,
                               "ticket must be 32 hex digits, seconds 1 to "
                               "86400");
      }
    }
    return ipc::make_result(
        request.id,
        std::string("{\"invite_only\":") +
            (xerces_join_password::invite_only() ? "true" : "false") + "}");
  }
  if (request.method == "punch") {
    std::string error;
    const auto address = text_param(request.params, "address", 64, &error);
    if (!address) {
      return ipc::make_error(request.id, ipc::invalid_params, error);
    }
    std::string response = on_main(request, [address]() -> std::string {
      return xerces_punch::start(*address) ? "{\"punching\":true}" : "!refused";
    });
    if (response.find("\"message\":\"refused\"") != std::string::npos) {
      return ipc::make_error(request.id, ipc::refused,
                             "not hosting, or not a public IPv4 address");
    }
    return response;
  }
  if (request.method == "quit") {
    // The launcher asks the player first (a join that needs other mods
    // restarts the game). The engine's own quit, as the console runs it, so
    // it counts as a clean exit (loader.cpp).
    return on_main(request, []() -> std::string {
      xerces_runtime::log_info("xerces.quit", "the launcher asked the game "
                                              "to quit");
      game::cbuf::Cbuf_AddText(game::LOCAL_CLIENT_0, "quit\n");
      return "{\"quitting\":true}";
    });
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
      xerces_perf::note_ipc_start();
      xerces_runtime::log_info("ipc.start", server.pipe_name());
    } else {
      xerces_runtime::log_error("ipc.failed", error);
    }
  }

  void pre_destroy() override { server.stop(); }
};
} // namespace xerces_ipc

REGISTER_COMPONENT(xerces_ipc::component)
