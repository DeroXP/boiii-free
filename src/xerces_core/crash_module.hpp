#pragma once

// Where a module in a crash's stack comes from, for the crash dialog's
// "the crash happened inside ..." line (src/client/component/error_help.cpp,
// exception.cpp). Pure: paths in, a verdict out, so it can be tested.
//
// Upstream's rule was "a module outside the game folder and outside Windows
// was loaded into the game by other software". Under Xerces the client
// (boiii.exe) sits next to Xerces.exe, not in the game folder, so that rule
// blamed the client's own hooks on an overlay or antivirus. The client's own
// module is the client, wherever its folder is.

#include <optional>
#include <string>
#include <string_view>

namespace xerces_core {
enum class module_origin {
  client,          // the running Xerces game client (boiii.exe), any folder
  overlay,         // a known overlay (Steam, Discord, RTSS, OBS, ...)
  graphics_driver, // under Windows' DriverStore
  game_proxy,      // a known proxy DLL (dxgi.dll, d3d11.dll, ...) in the
                   // game folder: ReShade or another mod loader
  game,            // anything else directly in the game folder
  directx,         // d3d11.dll, dxgi.dll or d3d9.dll in the Windows folder
  windows,         // anything else in the Windows folder
  other_software,  // anything else: loaded into the game by other software
};

const char *to_string(module_origin origin);

// module_path: the module's full path. client_path: the running client's
// own module (boiii.exe, wherever it is). game_dir: the Black Ops III folder.
// windows_dir: the Windows folder (GetWindowsDirectory). Paths compare
// without regard to ASCII case, '/' and '\' alike, with or without a "\\?\"
// prefix or a trailing separator. An empty client_path or game_dir matches
// nothing.
module_origin classify_module(std::string_view module_path,
                              std::string_view client_path,
                              std::string_view game_dir,
                              std::string_view windows_dir);

// The overlay a module file name belongs to ("the Steam overlay" for
// gameoverlayrenderer64.dll), any case; nullopt for other names.
std::optional<std::string_view> overlay_name(std::string_view file_name);

// The file name of a path, lower-cased (ASCII), for the dialog's text.
std::string module_file_name(std::string_view module_path);
} // namespace xerces_core
