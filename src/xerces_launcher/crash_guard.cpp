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
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <set>

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
LPTOP_LEVEL_EXCEPTION_FILTER previous_filter = nullptr;

// The report is written by a thread made at the start (a review,
// 2026-11-14): it has a stack of its own, which a stack overflow leaves the
// crashed thread too little of, and the crashed thread waits for it only so
// long, so a dump stuck on a lock a stopped thread holds cannot keep the
// launcher hanging. One crash is reported; a second thread that crashes
// meanwhile waits for that report before the process ends.
std::atomic<bool> handling{false};
HANDLE report_wanted = nullptr;
HANDLE report_done = nullptr;
EXCEPTION_POINTERS *report_info = nullptr;
const char *report_what = nullptr;
DWORD report_thread = 0;
constexpr DWORD report_wait_ms = 10'000;

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
// The writer's (one report is written at a time)
text_buffer text;
char what_text[512]{};

// The module an address is in: its file name and base. GetModuleHandleExW
// with an address asks the loader: a crash that holds the loader's lock makes
// this wait, which the crashed thread's time limit ends.
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

void add_place(const DWORD64 address) {
  char name[MAX_PATH]{};
  DWORD64 base = 0;
  if (module_of(address, name, base)) {
    text.add("%s+0x%llX", name,
             static_cast<unsigned long long>(address - base));
  } else {
    text.add("0x%llX", static_cast<unsigned long long>(address));
  }
}

// The stack from a context, by unwinding: code without unwind data is taken
// as a leaf (its return address on top), and so is a call through a null
// pointer (rip 0), the commonest "ran code at 0x0".
__declspec(noinline) std::size_t walk(CONTEXT context,
                                      DWORD64 (&pcs)[max_frames]) {
  std::size_t count = 0;
  if (context.Rip == 0 && context.Rsp != 0) {
    pcs[count++] = 0;
    context.Rip = *reinterpret_cast<const DWORD64 *>(context.Rsp);
    context.Rsp += 8;
  }
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

// The text and, with dump set, the dump, into folder; thread: the crashed
// thread's ID. Returns whether the text was written; its path in path.
bool write_report(const wchar_t *folder, EXCEPTION_POINTERS *info,
                  const char *what, const DWORD thread, const bool dump,
                  wchar_t (&path)[MAX_PATH]) {
  SYSTEMTIME now{};
  GetLocalTime(&now);
  wchar_t stamp[32]{};
  swprintf_s(stamp, L"%04u-%02u-%02u-%02u-%02u-%02u", now.wYear, now.wMonth,
             now.wDay, now.wHour, now.wMinute, now.wSecond);
  swprintf_s(path, L"%s\\launcher-%s.txt", folder, stamp);

  text.length = 0;
  text.data[0] = '\0';
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
  add_place(address);
  text.add(" on thread %lu.\r\n", thread);
  if (record && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      record->NumberParameters >= 2) {
    const ULONG_PTR kind = record->ExceptionInformation[0];
    text.add("It tried to %s address 0x%llX.\r\n",
             kind == 0   ? "read"
             : kind == 1 ? "write"
                         : "run code at",
             static_cast<unsigned long long>(record->ExceptionInformation[1]));
  }
  if (record && record->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
    text.add("The thread ran out of stack.\r\n");
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
    add_place(pcs[i]);
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
      exception.ThreadId = thread;
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

void write_guarded() {
  __try {
    wchar_t path[MAX_PATH]{};
    write_report(crash_folder, report_info, report_what, report_thread, true,
                 path);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

DWORD WINAPI report_writer(LPVOID) {
  WaitForSingleObject(report_wanted, INFINITE);
  write_guarded();
  SetEvent(report_done);
  return 0;
}

// The crashed thread's side: hands the crash to the writer and waits for it,
// at most report_wait_ms. A thread that crashes while another's report is
// written waits for that one. Returns whether this thread asked for the
// report.
bool report(EXCEPTION_POINTERS *info, const char *what) {
  if (crash_folder[0] == L'\0' || !report_wanted || !report_done) {
    return false;
  }
  if (handling.exchange(true)) {
    WaitForSingleObject(report_done, report_wait_ms);
    return false;
  }
  report_info = info;
  report_what = what;
  report_thread = GetCurrentThreadId();
  SetEvent(report_wanted);
  WaitForSingleObject(report_done, report_wait_ms);
  return true;
}

// A C++ exception that reaches the top on the main thread goes on to the
// runtime's own filter, which calls std::terminate: on_terminate reports it
// with its message. Everything else is reported here.
constexpr DWORD cpp_exception = 0xE06D7363;

LONG WINAPI crash_filter(EXCEPTION_POINTERS *info) {
  const bool cpp = info && info->ExceptionRecord &&
                   info->ExceptionRecord->ExceptionCode == cpp_exception;
  if (!cpp || !previous_filter) {
    report(info, nullptr);
  }
  return previous_filter ? previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

// A report from where the thread is now, for the ends that raise no
// exception: std::terminate, abort, the runtime's invalid-parameter and
// pure-call checks. what is copied: it may be on this thread's stack.
void report_here(const char *what) {
  strncpy_s(what_text, what, _TRUNCATE);
  CONTEXT context{};
  RtlCaptureContext(&context);
  EXCEPTION_RECORD record{};
  record.ExceptionCode = cpp_exception;
  record.ExceptionAddress = reinterpret_cast<PVOID>(context.Rip);
  EXCEPTION_POINTERS pointers{&record, &context};
  report(&pointers, what_text);
}

// std::terminate on a thread that has this handler (the runtime keeps one
// per thread: worker threads end through abort, on_abort).
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
  report_here(what);
  std::abort();
}

// abort() raises SIGABRT before it ends the process, and that handler is the
// whole process's: an uncaught exception on a worker thread (std::thread's
// body is noexcept) ends here.
void on_abort(int) {
  report_here("abort (an uncaught C++ exception on a worker thread, or a "
              "call of abort)");
}

void on_invalid_parameter(const wchar_t *, const wchar_t *, const wchar_t *,
                          unsigned int, uintptr_t) {
  report_here("an invalid parameter to a C runtime function");
  std::abort();
}

void on_pure_call() {
  report_here("a call of a pure virtual function");
  std::abort();
}

std::string utf8(const std::filesystem::path &path) {
  const std::u8string text = path.u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

// The names already said, kept outside the crash folder so Diagnostics' crash
// list shows only reports
std::filesystem::path seen_file_of(const std::filesystem::path &crash_dir) {
  return crash_dir.parent_path() / "cache" / "launcher-crashes-seen.txt";
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
  report_wanted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  report_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (const HANDLE writer = CreateThread(nullptr, 64 * 1024, report_writer,
                                         nullptr, 0, nullptr)) {
    CloseHandle(writer);
  } else {
    report_wanted = nullptr; // no writer: no reports
  }
  previous_filter = SetUnhandledExceptionFilter(crash_filter);
  std::set_terminate(on_terminate);
  std::signal(SIGABRT, on_abort);
  _set_invalid_parameter_handler(on_invalid_parameter);
  _set_purecall_handler(on_pure_call);
}

std::vector<previous_crash>
take_previous_crashes(const std::filesystem::path &crash_dir) {
  std::vector<previous_crash> out;
  std::set<std::string> seen;
  {
    std::ifstream in(seen_file_of(crash_dir));
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
        seen.contains(name)) {
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
  std::ranges::sort(out, {}, &previous_crash::file);
  return out;
}

void mark_crashes_seen(const std::filesystem::path &crash_dir,
                       const std::vector<previous_crash> &crashes) {
  if (crashes.empty()) {
    return;
  }
  const std::filesystem::path seen_file = seen_file_of(crash_dir);
  std::error_code ec;
  std::filesystem::create_directories(seen_file.parent_path(), ec);
  std::ofstream append(seen_file, std::ios::app | std::ios::binary);
  for (const previous_crash &crash : crashes) {
    append << crash.file << "\r\n";
  }
}
} // namespace xerces_launcher
