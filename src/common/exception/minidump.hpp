#pragma once

#include <string>
#include <windows.h>

namespace exception {
std::string create_minidump(LPEXCEPTION_POINTERS exceptioninfo);

// Xerces: a smaller dump (threads, stacks and the memory they point to)
// written straight to `path`, with nothing read back into the crashing
// process. False if it could not be written.
bool write_minidump_file(LPEXCEPTION_POINTERS exceptioninfo,
                         const wchar_t *path);
} // namespace exception
