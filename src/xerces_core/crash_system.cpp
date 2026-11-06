// The PC's facts for a crash report (crash_report.hpp) and the random ids.
// Kept apart from crash_report.cpp, which the game client links: DXGI and
// the display calls stay out of boiii.exe's imports.

#include "crash_report.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bcrypt.h>
#include <dxgi.h>
#include <intrin.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <set>
#include <utility>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

namespace xerces_core {
namespace {
std::string utf8(const wchar_t *text) {
  const int length = static_cast<int>(wcslen(text));
  if (length == 0) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0,
                                       nullptr, nullptr);
  std::string out(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), size, nullptr,
                      nullptr);
  return out;
}

std::string trim(std::string text) {
  const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\0'; };
  while (!text.empty() && space(text.back())) {
    text.pop_back();
  }
  std::size_t start = 0;
  while (start < text.size() && space(text[start])) {
    ++start;
  }
  return text.substr(start);
}

std::string registry_string(const wchar_t *key, const wchar_t *name) {
  wchar_t buffer[512]{};
  DWORD size = sizeof(buffer);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ, nullptr,
                   buffer, &size) != ERROR_SUCCESS) {
    return {};
  }
  return utf8(buffer);
}

// CPUID's brand string, for a PC whose registry has no processor name.
std::string cpuid_brand() {
  int info[4]{};
  __cpuid(info, static_cast<int>(0x80000000u));
  if (static_cast<unsigned>(info[0]) < 0x80000004u) {
    return {};
  }
  std::array<char, 49> brand{};
  for (int leaf = 0; leaf < 3; ++leaf) {
    __cpuid(info, static_cast<int>(0x80000002u + static_cast<unsigned>(leaf)));
    std::memcpy(brand.data() + leaf * 16, info, 16);
  }
  return trim(std::string(brand.data()));
}
} // namespace

std::string new_report_id() {
  std::array<unsigned char, 16> bytes{};
  if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    return {};
  }
  std::string out;
  for (const unsigned char b : bytes) {
    char part[3];
    std::snprintf(part, sizeof(part), "%02x", b);
    out += part;
  }
  return out;
}

std::string format_driver_version(std::uint64_t version) {
  char text[64];
  std::snprintf(text, sizeof(text), "%u.%u.%u.%u",
                static_cast<unsigned>((version >> 48) & 0xFFFF),
                static_cast<unsigned>((version >> 32) & 0xFFFF),
                static_cast<unsigned>((version >> 16) & 0xFFFF),
                static_cast<unsigned>(version & 0xFFFF));
  return text;
}

crash_system collect_crash_system() {
  crash_system out;
  // RtlGetVersion tells the truth where GetVersionEx is shimmed.
  using rtl_get_version = LONG(WINAPI *)(OSVERSIONINFOW *);
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    if (auto get = reinterpret_cast<rtl_get_version>(
            reinterpret_cast<void *>(GetProcAddress(ntdll, "RtlGetVersion")))) {
      OSVERSIONINFOW info{};
      info.dwOSVersionInfoSize = sizeof(info);
      if (get(&info) == 0) {
        out.windows = std::to_string(info.dwMajorVersion) + "." +
                      std::to_string(info.dwMinorVersion) + "." +
                      std::to_string(info.dwBuildNumber);
      }
    }
  }
  out.cpu = trim(
      registry_string(L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      L"ProcessorNameString"));
  if (out.cpu.empty()) {
    out.cpu = cpuid_brand();
  }
  out.logical_cpus =
      static_cast<std::int64_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (GlobalMemoryStatusEx(&memory)) {
    out.ram_mib =
        static_cast<std::int64_t>(memory.ullTotalPhys / (1024 * 1024));
  }

  // The adapters as DXGI lists them; the driver version is the user-mode
  // driver's, which is what vendors' release notes name.
  IDXGIFactory1 *factory = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                   reinterpret_cast<void **>(&factory)))) {
    std::set<std::pair<std::string, std::string>> seen;
    IDXGIAdapter1 *adapter = nullptr;
    for (UINT i = 0;
         out.gpus.size() < 8 &&
         factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
         ++i) {
      DXGI_ADAPTER_DESC1 desc{};
      if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
          !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        crash_gpu gpu;
        gpu.name = trim(utf8(desc.Description));
        LARGE_INTEGER version{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),
                                                     &version))) {
          gpu.driver = format_driver_version(
              static_cast<std::uint64_t>(version.QuadPart));
        }
        if (seen.insert({gpu.name, gpu.driver}).second) {
          out.gpus.push_back(std::move(gpu));
        }
      }
      adapter->Release();
      adapter = nullptr;
    }
    factory->Release();
  }

  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
    out.display = std::to_string(mode.dmPelsWidth) + "x" +
                  std::to_string(mode.dmPelsHeight) + "@" +
                  std::to_string(mode.dmDisplayFrequency);
  }
  return out;
}
} // namespace xerces_core
