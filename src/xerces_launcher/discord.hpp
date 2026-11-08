#pragma once

// Discord (docs/DESIGN.md 10.12): the game on the player's Discord profile,
// and Ask to Join, over the Discord client's local connection (the vendored
// MIT discord-rpc; nothing goes to Discord's servers from here, only to the
// Discord app on this PC, which shows it to the player's Discord friends).
//
// Off until the player turns on "Show on Discord" in Friends. While the game
// runs, the profile shows its mode and whether the lobby is open; while the
// lobby is open and the player is signed in to Xerces, the presence carries
// a party and a join secret (the player's invite link), so Discord friends
// get Ask to Join. The host answers in Xerces; a player let in gets the
// secret from Discord, and their Xerces asks to join through the link, which
// the host's Xerces lets in at once for the next three minutes.
//
// Turning it on writes HKCU\Software\Classes\discord-<application id> (what
// Discord starts for a join when Xerces is closed: this Xerces and its home);
// turning it off deletes that key.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

struct DiscordUser; // discord_rpc.h

namespace xerces_launcher {
// The Xerces application on Discord's developer portal (2026-11-07).
inline constexpr const char *discord_application_id = "1568827390010859623";

struct discord_activity {
  std::string details;      // "Zombies"
  std::string state;        // "In the lobby"
  std::int64_t started = 0; // Unix seconds, or 0
  // Ask to Join: all three, or none.
  std::string party_id;
  int party_size = 0;
  int party_max = 0;
  std::string join_secret;
  bool operator==(const discord_activity &) const = default;
};

class discord_presence {
public:
  struct handlers {
    // Someone asks to join (the host): their Discord id and name. Answer
    // with respond() within 30 seconds, or Discord drops the request.
    std::function<void(const std::string &user_id, const std::string &name)>
        join_request;
    // This player was let in: the host's join secret.
    std::function<void(const std::string &secret)> join_game;
    // Connected to the Discord app (true), or lost it (false).
    std::function<void(bool)> connection;
  };

  discord_presence() = default;
  discord_presence(const discord_presence &) = delete;
  discord_presence &operator=(const discord_presence &) = delete;
  ~discord_presence();

  // Registers the launch command and connects (retrying in the background
  // while Discord is closed). One instance at a time.
  void start(const std::filesystem::path &program,
             const std::filesystem::path &home, handlers callbacks);
  // Clears the presence and disconnects; with forget, also deletes the
  // launch command's key.
  void stop(bool forget);
  bool started() const { return started_; }
  bool connected() const { return connected_; }

  // The activity to show, or nullopt for none; sent only when it changes.
  void show(const std::optional<discord_activity> &activity);
  void respond(const std::string &user_id, bool let_in);
  // Runs Discord's callbacks; call on every pass of the UI loop.
  void pump();

private:
  static void on_ready(const DiscordUser *user);
  static void on_disconnected(int code, const char *message);
  static void on_errored(int code, const char *message);
  static void on_join_game(const char *secret);
  static void on_join_request(const DiscordUser *user);

  bool started_ = false;
  bool connected_ = false;
  handlers handlers_;
  std::optional<discord_activity> shown_;
  bool shown_once_ = false;
};
} // namespace xerces_launcher
