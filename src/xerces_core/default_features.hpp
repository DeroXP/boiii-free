#pragma once

// Client features that are on unless their off flag is given (docs/DESIGN.md
// 12.4; on by default since 2026-11-14, the user's call): faster loads
// (src/client/xerces/fast_loads.cpp) and the Window and Input threads' waits
// that end as soon as what they wait for comes (present_wake.cpp). Each is
// off when its flag is on the game's command line before the first "+"
// command, spelled "-name" or "--name": the launcher's Settings switches add
// them, and safe mode always does (preview_launch; the culprit search's tests
// keep the features as the crashed launch had them). The flags that turned them
// on while they were opt-in
// (-xerces-fast-loads, -xerces-present-wake, -xerces-input-wake) change
// nothing now.

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "launch.hpp"

namespace xerces_core::default_features {
// Names without the dash, as the launcher's settings handle flags
// (settings_options' has_flag and with_flag)
inline constexpr std::string_view no_fast_loads = "xerces-no-fast-loads";
inline constexpr std::string_view no_present_wake = "xerces-no-present-wake";
inline constexpr std::string_view no_input_wake = "xerces-no-input-wake";
inline constexpr std::array<std::string_view, 3> off_names{
    no_fast_loads, no_present_wake, no_input_wake};

// Whether the feature whose off flag is `off_name` is on for these arguments
inline bool on(const std::vector<std::string> &args,
               const std::string_view off_name) {
  const std::string flag = "-" + std::string(off_name);
  return !has_launch_flag(args, flag) && !has_launch_flag(args, "-" + flag);
}
} // namespace xerces_core::default_features
