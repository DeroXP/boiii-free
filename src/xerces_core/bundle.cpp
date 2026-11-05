#include "bundle.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <set>

#include "journal.hpp"
#include "json.hpp"
#include "launch.hpp"
#include "log.hpp"
#include "version.hpp"

#undef GetObject

// Display devices for system.json; the headless programs link nothing else
// from user32.
#pragma comment(lib, "user32.lib")

namespace xerces_core {
namespace {
void put16(std::string &out, std::uint16_t value) {
  out.push_back(static_cast<char>(value & 0xFF));
  out.push_back(static_cast<char>(value >> 8));
}

void put32(std::string &out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
  }
}

// 1980-01-01 00:00 for every entry, so the same inputs make the same zip.
constexpr std::uint16_t dos_time = 0;
constexpr std::uint16_t dos_date = (0 << 9) | (1 << 5) | 1;
constexpr std::uint16_t utf8_names = 0x0800;

std::string read_text(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string utf8(const std::wstring &text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  std::string out(std::size_t(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(),
                      size, nullptr, nullptr);
  return out;
}

std::string registry_string(HKEY root, const wchar_t *key, const wchar_t *name) {
  wchar_t buffer[512]{};
  DWORD size = sizeof(buffer);
  if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, buffer, &size) !=
      ERROR_SUCCESS) {
    return {};
  }
  return utf8(buffer);
}
} // namespace

std::uint32_t crc32(std::string_view bytes) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> out{};
    for (std::uint32_t n = 0; n < 256; ++n) {
      std::uint32_t c = n;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      out[n] = c;
    }
    return out;
  }();
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char c : bytes) {
    crc = table[(crc ^ static_cast<std::uint8_t>(c)) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

void zip_writer::add(std::string_view name, std::string_view bytes) {
  entry item;
  item.name = std::string(name);
  item.crc = crc32(bytes);
  item.size = static_cast<std::uint32_t>(bytes.size());
  item.offset = static_cast<std::uint32_t>(data_.size());
  put32(data_, 0x04034b50);
  put16(data_, 20);
  put16(data_, utf8_names);
  put16(data_, 0); // stored
  put16(data_, dos_time);
  put16(data_, dos_date);
  put32(data_, item.crc);
  put32(data_, item.size);
  put32(data_, item.size);
  put16(data_, static_cast<std::uint16_t>(item.name.size()));
  put16(data_, 0);
  data_ += item.name;
  data_.append(bytes.data(), bytes.size());
  entries_.push_back(std::move(item));
}

std::string zip_writer::finish() const {
  std::string out = data_;
  const auto directory_offset = static_cast<std::uint32_t>(out.size());
  for (const entry &item : entries_) {
    put32(out, 0x02014b50);
    put16(out, 20);
    put16(out, 20);
    put16(out, utf8_names);
    put16(out, 0);
    put16(out, dos_time);
    put16(out, dos_date);
    put32(out, item.crc);
    put32(out, item.size);
    put32(out, item.size);
    put16(out, static_cast<std::uint16_t>(item.name.size()));
    put16(out, 0);
    put16(out, 0);
    put16(out, 0);
    put16(out, 0);
    put32(out, 0);
    put32(out, item.offset);
    out += item.name;
  }
  const auto directory_size =
      static_cast<std::uint32_t>(out.size()) - directory_offset;
  put32(out, 0x06054b50);
  put16(out, 0);
  put16(out, 0);
  put16(out, static_cast<std::uint16_t>(entries_.size()));
  put16(out, static_cast<std::uint16_t>(entries_.size()));
  put32(out, directory_size);
  put32(out, directory_offset);
  put16(out, 0);
  return out;
}

std::string system_info_json() {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "xerces", xerces_version_text, alloc);

  // RtlGetVersion tells the truth where GetVersionEx is shimmed.
  using rtl_get_version = LONG(WINAPI *)(OSVERSIONINFOW *);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (auto get = reinterpret_cast<rtl_get_version>(reinterpret_cast<void *>(
          GetProcAddress(ntdll, "RtlGetVersion")))) {
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (get(&info) == 0) {
      json::set(doc, "windows",
                std::to_string(info.dwMajorVersion) + "." +
                    std::to_string(info.dwMinorVersion) + "." +
                    std::to_string(info.dwBuildNumber),
                alloc);
    }
  }
  using wine_version = const char *(*)();
  if (auto wine = reinterpret_cast<wine_version>(reinterpret_cast<void *>(
          GetProcAddress(ntdll, "wine_get_version")))) {
    json::set(doc, "wine", wine(), alloc); // Proton runs on Wine (17.2)
  }

  json::set(doc, "cpu",
            registry_string(HKEY_LOCAL_MACHINE,
                            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                            L"ProcessorNameString"),
            alloc);
  json::set(doc, "logical_processors",
            static_cast<std::int64_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)),
            alloc);
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (GlobalMemoryStatusEx(&memory)) {
    json::set(doc, "ram_mb",
              static_cast<std::int64_t>(memory.ullTotalPhys / (1024 * 1024)),
              alloc);
  }

  std::set<std::string> gpus;
  rapidjson::Value displays(rapidjson::kArrayType);
  DISPLAY_DEVICEW device{};
  device.cb = sizeof(device);
  for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); ++i) {
    gpus.insert(utf8(device.DeviceString));
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if ((device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) &&
        EnumDisplaySettingsW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
      rapidjson::Value display(rapidjson::kObjectType);
      json::set(display, "width", static_cast<std::int64_t>(mode.dmPelsWidth),
                alloc);
      json::set(display, "height", static_cast<std::int64_t>(mode.dmPelsHeight),
                alloc);
      json::set(display, "hz",
                static_cast<std::int64_t>(mode.dmDisplayFrequency), alloc);
      json::set(display, "primary",
                (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0, alloc);
      displays.PushBack(display, alloc);
    }
    device = {};
    device.cb = sizeof(device);
  }
  json::set(doc, "gpus",
            json::string_array(std::vector<std::string>(gpus.begin(), gpus.end()),
                               alloc),
            alloc);
  json::set(doc, "displays", std::move(displays), alloc);
  return json::pretty(doc);
}

bundle_result make_bundle(const std::filesystem::path &home,
                          const std::filesystem::path &out,
                          std::string_view profile,
                          std::string_view system_json, std::size_t max_logs) {
  bundle_result result;
  result.file = out;
  zip_writer zip;
  std::string listing =
      "Xerces diagnostics bundle. Nothing in it was sent anywhere; share it\n"
      "only if you want to. The user folder is written as %USERPROFILE% and\n"
      "server addresses as " +
      std::string(redacted_address) +
      ". launcher.json has no player name, and only\n"
      "counts of the favourite and recent servers.\n\n";
  const auto add = [&](const std::string &name, std::string text) {
    // Logs written by older builds were never scrubbed of addresses, and a
    // file a person edited may hold one anywhere, so every file is scrubbed
    // again here.
    text = redact_addresses(redact_profile(text, profile));
    listing += name + "  (" + std::to_string(text.size()) + " bytes)\n";
    zip.add(name, text);
    result.contents.push_back(name);
  };

  // The newest logs of every role.
  std::error_code ec;
  std::vector<std::filesystem::directory_entry> logs;
  for (const auto &file : std::filesystem::directory_iterator(home / "logs", ec)) {
    if (file.path().extension() == ".jsonl") {
      logs.push_back(file);
    }
  }
  std::ranges::sort(logs, [](const auto &a, const auto &b) {
    std::error_code e;
    return a.last_write_time(e) > b.last_write_time(e);
  });
  for (std::size_t i = 0; i < logs.size() && i < max_logs; ++i) {
    add("logs/" + utf8(logs[i].path().filename().wstring()),
        read_text(logs[i].path()));
  }

  const auto journal_path = home / "logs" / "journal.json";
  if (std::filesystem::exists(journal_path, ec)) {
    const std::string journal_text = read_text(journal_path);
    add("journal.json", journal_text);
    if (const auto journal = parse_journal(journal_text);
        journal && !journal->plan_id.empty()) {
      const std::string id = plan_file_id(journal->plan_id);
      const bool hex = std::ranges::all_of(id, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      });
      const auto plan = home / "config" / "plans" / (id + ".json");
      if (hex && std::filesystem::exists(plan, ec)) {
        add("plan.json", read_text(plan));
      }
    }
  }
  const auto report = home / "cache" / "signatures-report.json";
  if (std::filesystem::exists(report, ec)) {
    add("signatures-report.json", read_text(report));
  }
  const auto config = home / "config" / "launcher.json";
  if (std::filesystem::exists(config, ec)) {
    auto parsed = json::parse(read_text(config));
    if (parsed.ok && parsed.document.IsObject()) {
      auto &doc = parsed.document;
      auto &alloc = doc.GetAllocator();
      doc.RemoveMember("player_name"); // not needed to debug
      // Favourites pair a name ("Dave's house") with an address, and recent
      // servers are addresses: a bug report only needs how many there are.
      for (const char *key : {"favorites", "recent_servers"}) {
        const auto *list = json::member(doc, key);
        const std::int64_t count = list != nullptr && list->IsArray()
                                       ? static_cast<std::int64_t>(list->Size())
                                       : 0;
        const bool present = list != nullptr;
        doc.RemoveMember(key);
        if (present) {
          json::set(doc, std::string(key) + "_count", count, alloc);
        }
      }
      add("launcher.json", json::pretty(doc));
    }
  }
  add("system.json", std::string(system_json));
  zip.add("FILES.txt", listing);
  result.contents.push_back("FILES.txt");

  std::filesystem::create_directories(out.parent_path(), ec);
  std::ofstream file(out, std::ios::binary | std::ios::trunc);
  const std::string bytes = zip.finish();
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!file) {
    result.error = "could not write the bundle";
    return result;
  }
  result.ok = true;
  return result;
}
} // namespace xerces_core
