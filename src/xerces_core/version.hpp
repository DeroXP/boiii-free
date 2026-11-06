#pragma once

namespace xerces_core {
// The release script sets the version (DESIGN.md 16.1).
inline constexpr const char *xerces_version_text = "0.1.0-dev";
// The CLI's command set (xerces_cli --api-version) and the join protocol.
inline constexpr int cli_api_version = 1;
inline constexpr unsigned protocol_version = 1;

// The git commit this build was made from, and whether the tree had changes
// then, from the build\src\version.h premake writes; "unknown" and false
// without it. Crash reports carry both (docs/CRASH_REPORTS.md).
const char *xerces_build_commit();
bool xerces_build_dirty();
} // namespace xerces_core
