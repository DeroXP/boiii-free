#pragma once

#include <functional>
#include <string_view>

#include "log.hpp"

namespace xerces_core {
// Where shared launcher code reports what it did (DESIGN.md 15.1). The event
// comes without its role: "launch.dry" becomes launcher.launch.dry in
// Xerces.exe's log and cli.launch.dry in xerces_cli's. data_json is a JSON
// object or empty. An empty event_log drops everything.
using event_log =
    std::function<void(log_level level, std::string_view event,
                       std::string_view message, std::string_view data_json)>;

inline void emit(const event_log &log, log_level level, std::string_view event,
                 std::string_view message, std::string_view data_json = {}) {
  if (log) {
    log(level, event, message, data_json);
  }
}
} // namespace xerces_core
