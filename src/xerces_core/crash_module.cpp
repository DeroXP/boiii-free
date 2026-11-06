#include "crash_module.hpp"

#include <array>
#include <utility>

namespace xerces_core {
namespace {
// Lower-case ASCII, '/' as '\', no "\\?\" prefix, no trailing separator.
std::string normal(std::string_view path) {
  std::string out;
  out.reserve(path.size());
  for (char c : path) {
    if (c == '/') {
      c = '\\';
    } else if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    out.push_back(c);
  }
  if (out.starts_with("\\\\?\\")) {
    out.erase(0, 4);
  }
  while (out.size() > 3 && out.back() == '\\') {
    out.pop_back();
  }
  return out;
}

std::string parent_of(const std::string &normalised) {
  const std::size_t slash = normalised.rfind('\\');
  return slash == std::string::npos ? std::string()
                                    : normalised.substr(0, slash);
}

std::string file_of(const std::string &normalised) {
  const std::size_t slash = normalised.rfind('\\');
  return slash == std::string::npos ? normalised : normalised.substr(slash + 1);
}

constexpr std::array<std::pair<std::string_view, std::string_view>, 6> overlays{
    {
        {"gameoverlayrenderer64.dll", "the Steam overlay"},
        {"discordhook64.dll", "the Discord overlay"},
        {"rtsshooks64.dll", "RivaTuner Statistics Server (MSI Afterburner)"},
        {"graphics-hook64.dll", "OBS game capture"},
        {"nvspcap64.dll", "the GeForce Experience overlay"},
        {"ow-graphics-hook64.dll", "the Overwolf overlay"},
    }};

constexpr std::array<std::string_view, 8> proxy_dlls{
    "dxgi.dll",    "d3d11.dll", "d3d9.dll",      "dinput8.dll",
    "version.dll", "winmm.dll", "xinput1_3.dll", "dsound.dll"};
} // namespace

const char *to_string(module_origin origin) {
  switch (origin) {
  case module_origin::client:
    return "client";
  case module_origin::overlay:
    return "overlay";
  case module_origin::graphics_driver:
    return "graphics_driver";
  case module_origin::game_proxy:
    return "game_proxy";
  case module_origin::game:
    return "game";
  case module_origin::directx:
    return "directx";
  case module_origin::windows:
    return "windows";
  case module_origin::other_software:
    return "other_software";
  }
  return "other_software";
}

std::string module_file_name(std::string_view module_path) {
  return file_of(normal(module_path));
}

std::optional<std::string_view> overlay_name(std::string_view file_name) {
  const std::string name = normal(file_name);
  for (const auto &[file, label] : overlays) {
    if (name == file) {
      return label;
    }
  }
  return std::nullopt;
}

module_origin classify_module(std::string_view module_path,
                              std::string_view client_path,
                              std::string_view game_dir,
                              std::string_view windows_dir) {
  const std::string path = normal(module_path);
  const std::string name = file_of(path);
  if (!client_path.empty() && path == normal(client_path)) {
    return module_origin::client;
  }
  if (overlay_name(name)) {
    return module_origin::overlay;
  }
  if (path.find("\\driverstore\\") != std::string::npos) {
    return module_origin::graphics_driver;
  }
  if (!game_dir.empty() && parent_of(path) == normal(game_dir)) {
    for (const std::string_view proxy : proxy_dlls) {
      if (name == proxy) {
        return module_origin::game_proxy;
      }
    }
    return module_origin::game;
  }
  const std::string windows = normal(windows_dir);
  if (!windows.empty() &&
      (path == windows || path.starts_with(windows + "\\"))) {
    if (name == "d3d11.dll" || name == "dxgi.dll" || name == "d3d9.dll") {
      return module_origin::directx;
    }
    return module_origin::windows;
  }
  return module_origin::other_software;
}
} // namespace xerces_core
