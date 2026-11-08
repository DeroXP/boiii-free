#include "discord.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <discord_register.h>
#include <discord_rpc.h>

#include "log.hpp"

namespace xerces_launcher {
namespace {
// discord-rpc calls back through plain functions: the one instance started.
discord_presence *active = nullptr;

std::string utf8(const std::wstring &text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr,
                                       0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), size, nullptr, nullptr);
  return out;
}

std::string key_path() {
  return std::string("Software\\Classes\\discord-") + discord_application_id;
}
} // namespace

discord_presence::~discord_presence() { stop(false); }

void discord_presence::start(const std::filesystem::path &program,
                             const std::filesystem::path &home,
                             handlers callbacks) {
  if (started_ || active) {
    return;
  }
  handlers_ = std::move(callbacks);
  active = this;
  // What Discord starts for a join while Xerces is closed: this Xerces, with
  // its home (the second start hands over to an open one).
  const std::string command = "\"" + utf8(program.wstring()) + "\" --home \"" +
                              utf8(home.wstring()) + "\"";
  Discord_Register(discord_application_id, command.c_str());
  DiscordEventHandlers events{};
  events.ready = &discord_presence::on_ready;
  events.disconnected = &discord_presence::on_disconnected;
  events.errored = &discord_presence::on_errored;
  events.joinGame = &discord_presence::on_join_game;
  events.joinRequest = &discord_presence::on_join_request;
  Discord_Initialize(discord_application_id, &events, 0, nullptr);
  started_ = true;
  shown_.reset();
  shown_once_ = false;
  log_info("launcher.discord.start", "connecting to the Discord app");
}

void discord_presence::stop(bool forget) {
  if (started_) {
    Discord_ClearPresence();
    Discord_RunCallbacks();
    Discord_Shutdown();
    started_ = false;
    connected_ = false;
    log_info("launcher.discord.stop", "disconnected from the Discord app");
  }
  if (active == this) {
    active = nullptr;
  }
  if (forget) {
    const std::string path = key_path();
    const LSTATUS status = RegDeleteTreeA(HKEY_CURRENT_USER, path.c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
      log_warn("launcher.discord.unregister",
               "the Discord launch command could not be removed (" +
                   std::to_string(status) + ")");
    }
  }
}

void discord_presence::show(const std::optional<discord_activity> &activity) {
  if (!started_ || (shown_once_ && activity == shown_)) {
    return;
  }
  shown_ = activity;
  shown_once_ = true;
  if (!activity) {
    Discord_ClearPresence();
    return;
  }
  DiscordRichPresence presence{};
  presence.details = activity->details.c_str();
  presence.state = activity->state.c_str();
  presence.startTimestamp = activity->started;
  // The art asset of that name on the developer portal (Rich Presence, Art
  // Assets); without one Discord shows no picture.
  presence.largeImageKey = "xerces";
  presence.largeImageText = "Xerces";
  if (!activity->join_secret.empty()) {
    presence.partyId = activity->party_id.c_str();
    presence.partySize = activity->party_size;
    presence.partyMax = activity->party_max;
    presence.partyPrivacy = DISCORD_PARTY_PRIVATE;
    presence.joinSecret = activity->join_secret.c_str();
  }
  Discord_UpdatePresence(&presence);
}

void discord_presence::respond(const std::string &user_id, bool let_in) {
  if (started_) {
    Discord_Respond(user_id.c_str(),
                    let_in ? DISCORD_REPLY_YES : DISCORD_REPLY_NO);
  }
}

void discord_presence::pump() {
  if (started_) {
    Discord_RunCallbacks();
  }
}

void discord_presence::on_ready(const DiscordUser *) {
  if (!active) {
    return;
  }
  active->connected_ = true;
  // Discord forgets the presence of a connection that went away.
  active->shown_once_ = false;
  log_info("launcher.discord.ready", "connected to the Discord app");
  if (active->handlers_.connection) {
    active->handlers_.connection(true);
  }
}

void discord_presence::on_disconnected(int code, const char *) {
  if (!active) {
    return;
  }
  active->connected_ = false;
  log_info("launcher.discord.disconnected",
           "the Discord app went away (" + std::to_string(code) + ")");
  if (active->handlers_.connection) {
    active->handlers_.connection(false);
  }
}

void discord_presence::on_errored(int code, const char *message) {
  log_warn("launcher.discord.error",
           std::to_string(code) + ": " + (message ? message : ""));
}

void discord_presence::on_join_game(const char *secret) {
  if (active && secret && active->handlers_.join_game) {
    active->handlers_.join_game(secret);
  }
}

void discord_presence::on_join_request(const DiscordUser *user) {
  if (active && user && user->userId && active->handlers_.join_request) {
    // The name is shown to the host, never logged.
    active->handlers_.join_request(user->userId,
                                   user->username ? user->username : "");
  }
}
} // namespace xerces_launcher
