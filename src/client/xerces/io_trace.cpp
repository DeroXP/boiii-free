#include <std_include.hpp>

// -xerces-io-trace (test only; docs/research/engine-re.md, "A map's load"):
// every file the game opens and every read it makes, through its own import
// pointers (as present_wake.cpp does for Sleep): CreateFileA/W with the file's
// name and flags, ReadFile (with or without OVERLAPPED; an overlapped one
// still under way is not seen done: the game does not import
// GetOverlappedResult) and ReadFileEx with its completion routine wrapped.
// `xerces_io_trace_dump` writes what was seen
// to <home>\io-trace.csv (one read a line: when it was asked for and when it
// was done, in ms from the game's start, the thread, the file, the offset and
// the size) and <home>\io-files.csv (each file's index, CreateFile flags and
// name). It is there to see what a map's load waits on: how big the reads
// are, how many are under way at once and how long each takes.
//
// Off unless the flag is given; it adds a lock and a map look-up to every
// read, so never for play.

#include <component/command.hpp>
#include <game/game.hpp>
#include <loader/component_loader.hpp>

#include <utils/hook.hpp>
#include <utils/nt.hpp>

#include <xerces_core/launch.hpp>

#include "runtime.hpp"

namespace xerces_io_trace {
namespace {
constexpr std::string_view flag = "-xerces-io-trace";

enum class read_kind : std::uint8_t { sync, overlapped, ex };

struct read_record {
  std::int64_t asked = 0; // QPC
  std::int64_t done = 0;  // QPC; 0 while not seen done
  std::uint64_t offset = 0;
  std::uint32_t size = 0;
  std::uint32_t bytes = 0; // read, as reported
  std::uint32_t file = 0;  // index into files, 0 for a handle not seen opened
  std::uint32_t thread = 0;
  read_kind kind = read_kind::sync;
  bool failed = false;
};

constexpr std::size_t max_reads = 400'000;
read_record *reads = nullptr;
std::atomic<std::uint32_t> read_count{0};
std::int64_t qpc_start = 0;
std::int64_t qpc_frequency = 1;

struct file_entry {
  std::string name;
  DWORD flags = 0;
};
std::mutex files_mutex;
std::unordered_map<HANDLE, std::uint32_t> file_of;
std::vector<file_entry> files{{"(not seen opened)", 0}};

// ReadFileEx's reads not done yet, with the game's routine
struct pending_read {
  std::uint32_t index = 0;
  LPOVERLAPPED_COMPLETION_ROUTINE routine = nullptr;
};
std::mutex pending_mutex;
std::unordered_map<LPOVERLAPPED, pending_read> pending;

using create_file_w_fn = HANDLE(WINAPI *)(LPCWSTR, DWORD, DWORD,
                                          LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                          HANDLE);
using create_file_a_fn = HANDLE(WINAPI *)(LPCSTR, DWORD, DWORD,
                                          LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                          HANDLE);
using read_file_fn = BOOL(WINAPI *)(HANDLE, LPVOID, DWORD, LPDWORD,
                                    LPOVERLAPPED);
using read_file_ex_fn = BOOL(WINAPI *)(HANDLE, LPVOID, DWORD, LPOVERLAPPED,
                                       LPOVERLAPPED_COMPLETION_ROUTINE);
create_file_w_fn game_create_file_w = nullptr;
create_file_a_fn game_create_file_a = nullptr;
read_file_fn game_read_file = nullptr;
read_file_ex_fn game_read_file_ex = nullptr;

std::int64_t now() {
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return counter.QuadPart;
}

void note_file(const HANDLE handle, std::string name, const DWORD flags) {
  if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
    return;
  }
  std::lock_guard lock(files_mutex);
  files.push_back({std::move(name), flags});
  file_of[handle] = static_cast<std::uint32_t>(files.size() - 1);
}

std::uint32_t file_index(const HANDLE handle) {
  std::lock_guard lock(files_mutex);
  const auto it = file_of.find(handle);
  return it == file_of.end() ? 0 : it->second;
}

read_record *new_read(const HANDLE file, const std::uint64_t offset,
                      const DWORD size, const read_kind kind,
                      std::uint32_t &index) {
  index = read_count.fetch_add(1, std::memory_order_relaxed);
  if (index >= max_reads) {
    return nullptr;
  }
  read_record &record = reads[index];
  record.asked = now();
  record.offset = offset;
  record.size = size;
  record.file = file_index(file);
  record.thread = GetCurrentThreadId();
  record.kind = kind;
  return &record;
}

std::uint64_t offset_of(const OVERLAPPED *overlapped) {
  return overlapped
             ? (static_cast<std::uint64_t>(overlapped->OffsetHigh) << 32) |
                   overlapped->Offset
             : 0;
}

HANDLE WINAPI create_file_w_stub(LPCWSTR name, DWORD access, DWORD share,
                                 LPSECURITY_ATTRIBUTES security,
                                 DWORD disposition, DWORD flags,
                                 HANDLE template_file) {
  const HANDLE handle = game_create_file_w(name, access, share, security,
                                           disposition, flags, template_file);
  // The game reads the open's error after it: kept across the noting
  const DWORD error = GetLastError();
  if (name) {
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
    std::string text(length > 0 ? static_cast<std::size_t>(length - 1) : 0,
                     '\0');
    if (length > 1) {
      WideCharToMultiByte(CP_UTF8, 0, name, -1, text.data(), length, nullptr,
                          nullptr);
    }
    note_file(handle, std::move(text), flags);
  }
  SetLastError(error);
  return handle;
}

HANDLE WINAPI create_file_a_stub(LPCSTR name, DWORD access, DWORD share,
                                 LPSECURITY_ATTRIBUTES security,
                                 DWORD disposition, DWORD flags,
                                 HANDLE template_file) {
  const HANDLE handle = game_create_file_a(name, access, share, security,
                                           disposition, flags, template_file);
  const DWORD error = GetLastError();
  if (name) {
    note_file(handle, name, flags);
  }
  SetLastError(error);
  return handle;
}

void WINAPI read_done_stub(const DWORD error, const DWORD bytes,
                           LPOVERLAPPED overlapped) {
  pending_read found{};
  {
    std::lock_guard lock(pending_mutex);
    if (const auto it = pending.find(overlapped); it != pending.end()) {
      found = it->second;
      pending.erase(it);
    }
  }
  if (found.index < max_reads && found.routine) {
    reads[found.index].done = now();
    reads[found.index].bytes = bytes;
    reads[found.index].failed = error != 0;
  }
  if (found.routine) {
    found.routine(error, bytes, overlapped);
  }
}

BOOL WINAPI read_file_ex_stub(HANDLE file, LPVOID buffer, DWORD size,
                              LPOVERLAPPED overlapped,
                              LPOVERLAPPED_COMPLETION_ROUTINE routine) {
  std::uint32_t index = 0;
  read_record *record =
      new_read(file, offset_of(overlapped), size, read_kind::ex, index);
  if (!record || !routine) {
    return game_read_file_ex(file, buffer, size, overlapped, routine);
  }
  {
    std::lock_guard lock(pending_mutex);
    pending[overlapped] = {index, routine};
  }
  const BOOL ok =
      game_read_file_ex(file, buffer, size, overlapped, &read_done_stub);
  if (!ok) {
    std::lock_guard lock(pending_mutex);
    pending.erase(overlapped);
    record->failed = true;
    record->done = now();
  }
  return ok;
}

BOOL WINAPI read_file_stub(HANDLE file, LPVOID buffer, DWORD size, LPDWORD read,
                           LPOVERLAPPED overlapped) {
  // A read without OVERLAPPED is at the file's own position, not asked for:
  // asking moves nothing but races another thread's read (a review,
  // 2026-11-14), so its offset is left 0
  std::uint32_t index = 0;
  const std::uint64_t offset = offset_of(overlapped);
  read_record *record =
      new_read(file, offset, size,
               overlapped ? read_kind::overlapped : read_kind::sync, index);
  const BOOL ok = game_read_file(file, buffer, size, read, overlapped);
  if (!record) {
    return ok;
  }
  const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
  // An overlapped read still under way stays "not seen done": the game does
  // not import GetOverlappedResult, and an entry kept for it could overwrite
  // a ReadFileEx's on a reused OVERLAPPED (a review, 2026-11-14)
  if (ok || !overlapped || error != ERROR_IO_PENDING) {
    record->done = now();
    record->bytes = read ? *read : 0;
    record->failed = !ok;
  }
  SetLastError(error);
  return ok;
}

double ms_of(const std::int64_t qpc) {
  return qpc == 0 ? -1.0
                  : static_cast<double>(qpc - qpc_start) * 1000.0 /
                        static_cast<double>(qpc_frequency);
}

void dump() {
  const std::uint32_t count =
      std::min<std::uint32_t>(read_count.load(), max_reads);
  std::string out = "asked_ms,done_ms,kind,thread,file,offset,size,bytes,"
                    "failed\n";
  out.reserve(static_cast<std::size_t>(count) * 64);
  for (std::uint32_t i = 0; i < count; ++i) {
    const read_record &r = reads[i];
    out += std::format(
        "{:.3f},{:.3f},{},{},{},{},{},{},{}\n", ms_of(r.asked), ms_of(r.done),
        r.kind == read_kind::ex           ? "ex"
        : r.kind == read_kind::overlapped ? "overlapped"
                                          : "sync",
        r.thread, r.file, r.offset, r.size, r.bytes, r.failed ? 1 : 0);
  }
  xerces_runtime::write_file(xerces_runtime::home() / "io-trace.csv", out);
  std::string names = "file,flags,name\n";
  {
    std::lock_guard lock(files_mutex);
    for (std::size_t i = 0; i < files.size(); ++i) {
      std::string name = files[i].name;
      std::ranges::replace(name, ',', ';');
      names += std::format("{},0x{:08X},{}\n", i, files[i].flags, name);
    }
  }
  xerces_runtime::write_file(xerces_runtime::home() / "io-files.csv", names);
  xerces_runtime::log_info(
      "xerces.io_trace.dump",
      std::format("{} reads and {} files written to io-trace.csv and "
                  "io-files.csv, at {:.0f} ms of the trace",
                  count, files.size(), ms_of(now())));
}

template <typename T> bool hook_import(const char *name, T stub, T &original) {
  void *entry = utils::nt::library{}.get_iat_entry("kernel32.dll", name);
  if (!entry) {
    return false;
  }
  original = *static_cast<T *>(entry);
  utils::hook::set(entry, stub);
  return true;
}
} // namespace

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_io_trace");

public:
  void post_unpack() override {
    const auto args = xerces_runtime::command_line_args();
    if (!xerces_core::has_launch_flag(args, flag) &&
        !xerces_core::has_launch_flag(args, "-" + std::string(flag))) {
      return;
    }
    reads = static_cast<read_record *>(
        VirtualAlloc(nullptr, sizeof(read_record) * max_reads,
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!reads) {
      return;
    }
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;
    qpc_start = now();
    const std::string hooked = std::format(
        "CreateFileW {}, CreateFileA {}, ReadFile {}, ReadFileEx {}",
        hook_import("CreateFileW", &create_file_w_stub, game_create_file_w),
        hook_import("CreateFileA", &create_file_a_stub, game_create_file_a),
        hook_import("ReadFile", &read_file_stub, game_read_file),
        hook_import("ReadFileEx", &read_file_ex_stub, game_read_file_ex));
    xerces_runtime::log_info("xerces.io_trace.on",
                             "The game's file reads are traced (" + hooked +
                                 "); xerces_io_trace_dump writes them");
    command::add("xerces_io_trace_dump", [] { dump(); });
  }
};
} // namespace xerces_io_trace

REGISTER_COMPONENT(xerces_io_trace::component)
