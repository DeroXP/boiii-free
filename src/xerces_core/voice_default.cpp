#include "voice_default.hpp"

#include "settings_options.hpp"

namespace xerces_core {
bool voice_push_to_talk_due(std::string_view config_text) {
  return !saved_dvar_on(config_text, voice_default_dvar).value_or(false);
}
} // namespace xerces_core
