#include "crash_guard.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <set>
#include <sstream>

namespace xerces_launcher {
namespace {
// Set at the start; read at a crash, when nothing new may be asked of the
// heap: everything below is fixed buffers.
wchar_t crash_folder[MAX_PATH]{};
char version_text[96]{};
using minidump_fn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                   PMINIDUMP_EXCEPTION_INFORMATION,
                                   PMINIDUMP_USER_STREAM_INFORMATION,
                                   PMINIDUMP_CALLBACK_INFORMATION);
minidump_fn write_minidump = nullptr;
std::atomic<bool> handling{false};
LPTOP_LEVEL_EXCEPTION_FILTER previous_filter = nullptr;

constexpr std::size_t max_frames = 48;

struct text_buffer {
  char data[16384]{};
  std::size_t length = 0;

  void add(const char *format, ...) {
    if (length + 1 >= sizeof(data)) {
      return;
    }
    va_list args;
    va_start(args, format);
    const int wrote =
        std::vsnprintf(data + length, sizeof(data) - length, format, args);
    va_end(args);
    if (wrote > 0) {
      length =
          std::min(sizeof(data) - 1, length + static_cast<std::size_t>(wrote));
    }
  }
};

// The module an address is in: its file name and base. GetModuleHandleExW
// with the address asks the loader, which a crash inside the loader may
// hold; the report is written by then and only the names are lost.
bool module_of(const DWORD64 address, char (&name)[MAX_PATH], DWORD64 &base) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &module)) {
    return false;
  }
  wchar_t path[MAX_PATH]{};
  if (!GetModuleFileNameW(module, path, MAX_PATH)) {
    return false;
  }
  const wchar_t *file = std::wcsrchr(path, L'\\');
  file = file ? file + 1 : path;
  if (!WideCharToMultiByte(CP_UTF8, 0, file, -1, name, MAX_PATH, nullptr,
                           nullptr)) {
    return false;
  }
  base = reinterpret_cast<DWORD64>(module);
  return true;
}

void add_place(text_buffer &text, const DWORD64 address) {
  char name[MAX_PATH]{};
  DWORD64 base = 0;
  if (module_of(address, name, base)) {
    text.add("%s+0x%llX", name,
             static_cast<unsigned long long>(address - base));
  } else {
    text.add("0x%llX", static_cast<unsigned long long>(address));
  }
}

// The stack from a context, by unwinding (the crashed thread's own): code
// without unwind data is taken as a leaf (its return address on top).
__declspec(noinline) std::size_t walk(CONTEXT context,
                                      DWORD64 (&pcs)[max_frames]) {
  std::size_t count = 0;
  while (count < max_frames && context.Rip != 0) {
    pcs[count++] = context.Rip;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function =
        RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    const DWORD64 before = context.Rsp;
    if (function) {
      PVOID handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function,
                       &context, &handler_data, &establisher, nullptr);
    } else {
      context.Rip = *reinterpret_cast<const DWORD64 *>(context.Rsp);
      context.Rsp += 8;
    }
    if (context.Rsp <= before) {
      break;
    }
  }
  return count;
}

std::size_t walk_guarded(const CONTEXT &context, DWORD64 (&pcs)[max_frames]) {
  __try {
    return walk(context, pcs);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

// The text and, with dump set, the dump; folder: where. Returns whether the
// text was written; its path in path.
bool write_report(const wchar_t *folder, EXCEPTION_POINTERS *info,
                  const char *what, const bool dump,
                  wchar_t (&path)[MAX_PATH]) {
  SYSTEMTIME now{};
  GetLocalTime(&now);
  wchar_t stamp[32]{};
  swprintf_s(stamp, L"%04u-%02u-%02u-%02u-%02u-%02u", now.wYear, now.wMonth,
             now.wDay, now.wHour, now.wMinute, now.wSecond);
  swprintf_s(path, L"%s\\launcher-%s.txt", folder, stamp);

  text_buffer text;
  text.add("Xerces Launcher Crash Report\r\n\r\n");
  const EXCEPTION_RECORD *record = info ? info->ExceptionRecord : nullptr;
  const DWORD64 address =
      record ? reinterpret_cast<DWORD64>(record->ExceptionAddress) : 0;
  if (what) {
    text.add("What happened: the launcher ended on %s, at ", what);
  } else {
    text.add("What happened: exception 0x%08lX at ",
             record ? record->ExceptionCode : 0ul);
  }
  add_place(text, address);
  text.add(" on thread %lu.\r\n", GetCurrentThreadId());
  if (record && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      record->NumberParameters >= 2) {
    const ULONG_PTR kind = record->ExceptionInformation[0];
    text.add("It tried to %s address 0x%llX.\r\n",
             kind == 0   ? "read"
             : kind == 1 ? "write"
                         : "run code at",
             static_cast<unsigned long long>(record->ExceptionInformation[1]));
  }
  text.add("\r\nVersion: Xerces %s\r\n", version_text);
  text.add("Time: %04u-%02u-%02u %02u:%02u:%02u (local)\r\n", now.wYear,
           now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
  text.add("Process: %lu\r\n\r\nStack:\r\n", GetCurrentProcessId());
  DWORD64 pcs[max_frames]{};
  const std::size_t frames =
      info && info->ContextRecord ? walk_guarded(*info->ContextRecord, pcs) : 0;
  for (std::size_t i = 0; i < frames; ++i) {
    text.add("  [%2zu] ", i);
    add_place(text, pcs[i]);
    text.add("\r\n");
  }
  if (frames == 0) {
    text.add("  (not read)\r\n");
  }
  bool written = false;
  const HANDLE file =
      CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    DWORD wrote = 0;
    written = WriteFile(file, text.data, static_cast<DWORD>(text.length),
                        &wrote, nullptr) != FALSE;
    CloseHandle(file);
  }
  if (dump && write_minidump) {
    wchar_t dump_path[MAX_PATH]{};
    swprintf_s(dump_path, L"%s\\launcher-%s.dmp", folder, stamp);
    const HANDLE dump_file =
        CreateFileW(dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump_file != INVALID_HANDLE_VALUE) {
      MINIDUMP_EXCEPTION_INFORMATION exception{};
      exception.ThreadId = GetCurrentThreadId();
      exception.ExceptionPointers = info;
      exception.ClientPointers = FALSE;
      write_minidump(GetCurrentProcess(), GetCurrentProcessId(), dump_file,
                     static_cast<MINIDUMP_TYPE>(MiniDumpNormal |
                                                MiniDumpWithThreadInfo |
                                                MiniDumpWithUnloadedModules),
                     info ? &exception : nullptr, nullptr, nullptr);
      CloseHandle(dump_file);
    }
  }
  return written;
}

void write_guarded(EXCEPTION_POINTERS *info, const char *what) {
  __try {
    wchar_t path[MAX_PATH]{};
    write_report(crash_folder, info, what, true, path);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS *info) {
  if (crash_folder[0] != L'\0' && !handling.exchange(true)) {
    write_guarded(info, nullptr);
  }
  return previous_filter ? previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

// An uncaught C++ exception ends in std::terminate and then abort, which no
// exception filter sees: the report is written here, from this thread's
// context.
[[noreturn]] void on_terminate() {
  char what[512] = "an uncaught C++ exception";
  if (const std::exception_ptr current = std::current_exception()) {
    try {
      std::rethrow_exception(current);
    } catch (const std::exception &error) {
      std::snprintf(what, sizeof(what), "an uncaught C++ exception (%s)",
                    error.what());
    } catch (...) {
    }
  } else {
    std::snprintf(what, sizeof(what), "std::terminate without an exception");
  }
  if (crash_folder[0] != L'\0' && !handling.exchange(true)) {
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = 0xE06D7363; // the C++ exception code
    record.ExceptionAddress = reinterpret_cast<PVOID>(context.Rip);
    EXCEPTION_POINTERS pointers{&record, &context};
    write_guarded(&pointers, what);
  }
  std::abort();
}

std::string utf8(const std::filesystem::path &path) {
  const std::u8string text = path.u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}
} // namespace

void install_crash_guard(const std::filesystem::path &crash_dir,
                         const std::string &version) {
  std::error_code ec;
  std::filesystem::create_directories(crash_dir, ec);
  const std::wstring folder = crash_dir.wstring();
  if (folder.size() + 48 >= MAX_PATH) {
    return; // the report's path would not fit: no guard rather than a cut one
  }
  wcsncpy_s(crash_folder, folder.c_str(), _TRUNCATE);
  strncpy_s(version_text, version.c_str(), _TRUNCATE);
  // Loaded now: loading it at a crash would ask the loader, which the crash
  // may hold
  if (const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll")) {
    write_minidump = reinterpret_cast<minidump_fn>(
        GetProcAddress(dbghelp, "MiniDumpWriteDump"));
  }
  previous_filter = SetUnhandledExceptionFilter(crash_filter);
  std::set_terminate(on_terminate);
}

std::vector<previous_crash>
take_previous_crashes(const std::filesystem::path &crash_dir) {
  std::vector<previous_crash> out;
  const std::filesystem::path seen_file = crash_dir / "launcher-seen.txt";
  std::set<std::string> seen;
  {
    std::ifstream in(seen_file);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (!line.empty()) {
        seen.insert(line);
      }
    }
  }
  std::error_code ec;
  for (std::filesystem::directory_iterator it(crash_dir, ec), end;
       !ec && it != end; it.increment(ec)) {
    const std::string name = utf8(it->path().filename());
    if (!name.starts_with("launcher-") || !name.ends_with(".txt") ||
        name == "launcher-seen.txt" || seen.contains(name)) {
      continue;
    }
    previous_crash crash;
    crash.file = name;
    std::ifstream in(it->path(), std::ios::binary);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.starts_with("What happened: ")) {
        crash.what = line.substr(15, 400);
        break;
      }
    }
    out.push_back(std::move(crash));
  }
  if (!out.empty()) {
    std::ofstream append(seen_file, std::ios::app | std::ios::binary);
    for (const previous_crash &crash : out) {
      append << crash.file << "\r\n";
    }
  }
  std::ranges::sort(out, {}, &previous_crash::file);
  return out;
}

} // namespace xerces_launcher
