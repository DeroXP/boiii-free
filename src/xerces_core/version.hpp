#pragma once

namespace xerces_core {
// The release script sets the version (DESIGN.md 16.1).
inline constexpr const char *xerces_version_text = "0.1.0-dev";
// The CLI's command set (xerces_cli --api-version) and the join protocol.
inline constexpr int cli_api_version = 1;
inline constexpr unsigned protocol_version = 1;
} // namespace xerces_core
