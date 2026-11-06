// The build's git commit for crash reports (version.hpp). premake writes
// build\src\version.h on every run whose `git describe` changed; the
// headless projects have that folder on their include path.

#include "version.hpp"

#if __has_include(<version.h>)
#include <version.h>
#endif

namespace xerces_core {
const char *xerces_build_commit() {
#ifdef GIT_HASH
  return GIT_HASH;
#else
  return "unknown";
#endif
}

bool xerces_build_dirty() {
#ifdef GIT_DIRTY
  return GIT_DIRTY != 0;
#else
  return false;
#endif
}
} // namespace xerces_core
