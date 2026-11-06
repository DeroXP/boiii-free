#pragma once

#include <game/symbols/sym_include.hpp>

namespace game {
namespace in {
WEAK symbol<bool()> IN_CanTalk{0x1422BB470, 0x0, 0x0};
WEAK symbol<bool()> IN_IsTalkKeyHeld{0x14131B480, 0x0, 0x0};
// Takes a local client number (IN_MouseEvent passes primaryLocalClientNum),
// not a controller index; both are 32-bit, so the call is the same.
WEAK symbol<void(LocalClientNum_t localClientNum, int32_t device, bool force)>
    IN_SetActiveDevice{0x1420E3410, 0x0, 0x0};
} // namespace in
} // namespace game
