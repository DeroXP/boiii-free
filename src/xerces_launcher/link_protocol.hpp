#pragma once

// xerces:// invite links (docs/DESIGN.md 10.12): Windows starts Xerces with
// "--join-link <link>" when the player opens one in a browser or chat. Off
// until the player turns it on in Friends; it is written for this Windows
// user only (HKCU\Software\Classes\xerces), points at this Xerces and its
// home, and turning it off removes it again. Nothing else in the registry is
// touched.

#include <filesystem>
#include <string>

namespace xerces_launcher::link_protocol {
enum class state {
  off,       // no xerces:// handler for this user
  here,      // it starts this Xerces, with this home
  elsewhere, // it starts another copy (or another home)
};
state current(const std::filesystem::path &program,
              const std::filesystem::path &home);
// The command Windows runs: "<program>" --home "<home>" --join-link "%1".
std::wstring command(const std::filesystem::path &program,
                     const std::filesystem::path &home);
bool turn_on(const std::filesystem::path &program,
             const std::filesystem::path &home, std::string *error);
bool turn_off(std::string *error);

// When the handler is on for this home but starts another Xerces.exe (each
// beta is unpacked to a folder of its own), it is pointed at this one; true
// when it was. A handler for another home is left alone.
bool follow_this_copy(const std::filesystem::path &program,
                      const std::filesystem::path &home);
} // namespace xerces_launcher::link_protocol
