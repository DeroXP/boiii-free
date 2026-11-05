#pragma once

// The signature registry resolved inside the game (docs/DESIGN.md 3.3 step 3,
// 4.3, 4.4): compat/signatures.json is matched against the mapped
// BlackOps3.exe once, the first time a Xerces feature asks, before that
// feature installs any hook. A feature whose signatures do not all resolve
// is turned off and says so; nothing falls back to fixed addresses.
//
// The result is cached in <home>\cache\signatures-<build>-<registry>.json,
// keyed by the game build and a hash of the registry text, and re-validated on
// the next start by bytes and by the registry's known addresses. It is
// reported to the launcher in <home>\cache\signatures-report.json.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace xerces_signatures {
// Resolves on first use; safe to call from any component's post_unpack.
bool feature_enabled(std::string_view feature);
// The absolute address of a resolved signature, if it resolved.
std::optional<std::uintptr_t> address(std::string_view id);
// Why a feature is off, in the launcher's wording (empty if it is on).
std::string feature_message(std::string_view feature,
                            std::string_view feature_label,
                            std::string_view still_works);
} // namespace xerces_signatures
