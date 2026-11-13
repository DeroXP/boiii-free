#include "link_protocol.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shlobj.h>

#include <system_error>

namespace xerces_launcher::link_protocol {
namespace {
constexpr wchar_t key_path[] = L"Software\\Classes\\xerces";
constexpr wchar_t command_path[] =
    L"Software\\Classes\\xerces\\shell\\open\\command";

std::string message_of(LSTATUS status) {
  return std::system_category().message(static_cast<int>(status));
}

bool set_string(HKEY key, const wchar_t *name, const std::wstring &value,
                std::string *error) {
  const LSTATUS status = RegSetValueExW(
      key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  if (status != ERROR_SUCCESS) {
    *error = message_of(status);
    return false;
  }
  return true;
}

bool write_key(const std::wstring &path, const wchar_t *name,
               const std::wstring &value, std::string *error) {
  HKEY key = nullptr;
  const LSTATUS status =
      RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) {
    *error = message_of(status);
    return false;
  }
  const bool ok = set_string(key, name, value, error);
  RegCloseKey(key);
  return ok;
}
} // namespace

std::wstring command(const std::filesystem::path &program,
                     const std::filesystem::path &home) {
  return L"\"" + program.wstring() + L"\" --home \"" + home.wstring() +
         L"\" --join-link \"%1\"";
}

state current(const std::filesystem::path &program,
              const std::filesystem::path &home) {
  wchar_t value[2048]{};
  DWORD size = sizeof(value);
  const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, command_path, nullptr,
                                      RRF_RT_REG_SZ, nullptr, value, &size);
  if (status != ERROR_SUCCESS) {
    return state::off;
  }
  return _wcsicmp(value, command(program, home).c_str()) == 0
             ? state::here
             : state::elsewhere;
}

bool turn_on(const std::filesystem::path &program,
             const std::filesystem::path &home, std::string *error) {
  const bool ok =
      write_key(key_path, nullptr, L"URL:Xerces invite link", error) &&
      write_key(key_path, L"URL Protocol", L"", error) &&
      write_key(std::wstring(key_path) + L"\\DefaultIcon", nullptr,
                L"\"" + program.wstring() + L"\",0", error) &&
      write_key(command_path, nullptr, command(program, home), error);
  if (ok) {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  }
  return ok;
}

bool follow_this_copy(const std::filesystem::path &program,
                      const std::filesystem::path &home) {
  wchar_t value[2048]{};
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, command_path, nullptr, RRF_RT_REG_SZ,
                   nullptr, value, &size) != ERROR_SUCCESS) {
    return false;
  }
  const std::wstring registered(value);
  if (_wcsicmp(registered.c_str(), command(program, home).c_str()) == 0) {
    return false;
  }
  const std::wstring tail =
      L"\" --home \"" + home.wstring() + L"\" --join-link \"%1\"";
  if (registered.size() < tail.size() ||
      _wcsicmp(registered.c_str() + registered.size() - tail.size(),
               tail.c_str()) != 0) {
    return false;
  }
  std::string error;
  return turn_on(program, home, &error);
}

bool turn_off(std::string *error) {
  const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, key_path);
  if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
    *error = message_of(status);
    return false;
  }
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return true;
}
} // namespace xerces_launcher::link_protocol
