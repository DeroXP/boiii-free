#pragma once

// Xerces' version (DESIGN.md 16.1). VERSION at the top of the repository sets
// it; premake writes it with the commit and whether the tree had changes into
// build/src/xerces_version.h (premake5.lua, generate_xerces_version), which
// boiii.exe, Xerces.exe and xerces_cli all read. Without that file (a build
// premake did not set up) the version says it is unknown.

#if __has_include("xerces_version.h")
#include "xerces_version.h"
#endif

#ifndef XERCES_VERSION
#define XERCES_VERSION "0.0.0-unknown"
#define XERCES_GIT_HASH ""
#define XERCES_GIT_SHORT ""
#define XERCES_GIT_DIRTY 0
#define XERCES_VERSION_LABEL "Xerces 0.0.0-unknown"
#endif

namespace xerces_core {
inline constexpr const char *xerces_version_text = XERCES_VERSION;
// The full commit hash the build was made from, empty when unknown.
inline constexpr const char *xerces_commit = XERCES_GIT_HASH;
// The tree had uncommitted changes: the commit alone is not the source.
inline constexpr bool xerces_dirty = XERCES_GIT_DIRTY != 0;
// "Xerces 0.1.0-beta.1 (3f09512d)", "-dirty" after the hash when the tree had
// changes. The game client shows the same text.
inline constexpr const char *xerces_version_label = XERCES_VERSION_LABEL;
// Where the source of this build is: the fork's tree at the commit.
inline constexpr const char *xerces_source_repository =
    "https://github.com/DeroXP/boiii-free";
// The CLI's command set (xerces_cli --api-version) and the join protocol.
inline constexpr int cli_api_version = 1;
inline constexpr unsigned protocol_version = 1;
// The commit a crash report names (docs/CRASH_REPORTS.md): xerces_commit, or
// "unknown" when the build does not know it. Reports carry xerces_dirty with
// it, so a report, About, Diagnostics and the game client name one build.
inline constexpr const char *xerces_report_commit =
    XERCES_GIT_HASH[0] != '\0' ? XERCES_GIT_HASH : "unknown";
} // namespace xerces_core
