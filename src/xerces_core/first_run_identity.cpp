// The identity-folder guard of the BOIII import (first_run_import.hpp).
//
// %LOCALAPPDATA%\boiii\user holds the player's identity key. Deciding whether
// a path leads there must not open anything in it, not even to read its
// attributes. std::filesystem's canonical() and weakly_canonical() are not
// used for that reason: they open the path to ask Windows for its final name,
// and through a junction that opens the identity folder itself. Instead the
// path is walked one name at a time. Each name is looked up in its parent
// folder's listing (FindFirstFileExW), which gives its long name and whether
// it is a link without opening it. A link is opened as a link
// (FILE_FLAG_OPEN_REPARSE_POINT) to read where it points, never followed.
// Every step is compared with the identity folder before the next one is
// looked up, so the walk stops at its door.
//
// The comparison is by name, so it only works where a name says which folder
// of this PC it is: on a drive. A share or a network drive can be this PC's
// own C:\ under another name, so a path through one is never "outside".

#include "first_run_import.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Windows.h>
#include <winioctl.h>

namespace xerces_core {
namespace fs = std::filesystem;

namespace {
// A path through more links than this is a loop: it is not followed further.
constexpr int max_links = 32;

bool starts_with_nocase(std::wstring_view text, std::wstring_view prefix) {
  // CompareStringOrdinal ignores case with the file system's own case table,
  // so non-English names compare the way Windows compares them.
  return text.size() >= prefix.size() &&
         CompareStringOrdinal(text.data(), int(prefix.size()), prefix.data(),
                              int(prefix.size()), TRUE) == CSTR_EQUAL;
}

std::wstring without_trailing_separators(const fs::path &path) {
  std::wstring text = path.native();
  std::ranges::replace(text, L'/', L'\\');
  while (!text.empty() && text.back() == L'\\') {
    text.pop_back();
  }
  return text;
}

// a == b or a inside b.
bool within(const fs::path &a, const fs::path &b) {
  const std::wstring inner = without_trailing_separators(a);
  const std::wstring outer = without_trailing_separators(b);
  if (outer.empty() || !starts_with_nocase(inner, outer)) {
    return false;
  }
  return inner.size() == outer.size() || inner[outer.size()] == L'\\';
}

bool is_drive(std::wstring_view text) {
  return text.size() >= 2 &&
         ((text[0] >= L'A' && text[0] <= L'Z') ||
          (text[0] >= L'a' && text[0] <= L'z')) &&
         text[1] == L':' && (text.size() == 2 || text[2] == L'\\');
}

// A name Windows allows in a folder. Anything else (a stream after ':', a
// wildcard that would make the listing match some other name) is not checked.
bool allowed_name(std::wstring_view name) {
  return !name.empty() && std::ranges::none_of(name, [](wchar_t c) {
    return c < 32 || std::wstring_view(L"\\/:*?\"<>|").find(c) !=
                         std::wstring_view::npos;
  });
}

// A path as the drive it is on ("C:\") and the names below it.
struct location {
  fs::path root;
  std::vector<std::wstring> names;

  fs::path joined() const {
    fs::path out = root;
    for (const std::wstring &name : names) {
      out /= name;
    }
    return out;
  }
};

// "C:\x\y" as written, split into its drive and names. Only paths on a drive
// are checked: a share ("\\server\share\", "UNC\server\share") can be this
// PC's own drive (\\localhost\C$, \\127.0.0.1\c$, a share of any folder above
// the identity folder), and its name does not say which folder of which PC it
// is. Other device paths (pipes, volumes by GUID) are not folders on a drive.
// Both are nullopt, which the walk reports as unknown.
std::optional<location> split(std::wstring_view text) {
  if (!is_drive(text)) {
    return std::nullopt;
  }
  location out;
  out.root = std::wstring(text.substr(0, 2)) + L"\\";
  std::wstring_view rest = text.substr(2);

  // "." and ".." are applied by name, as Windows applies them before it looks
  // anything up (so "link\.." is the folder holding the link, not the link
  // target's parent). In a "\\?\" path the file system refuses them instead,
  // so applying them can only make the check stricter.
  while (!rest.empty()) {
    const std::size_t end = rest.find(L'\\');
    const std::wstring_view name = rest.substr(0, end);
    rest.remove_prefix(end == std::wstring_view::npos ? rest.size() : end + 1);
    if (name.empty() || name == L".") {
      continue;
    }
    if (name == L"..") {
      if (!out.names.empty()) {
        out.names.pop_back();
      }
      continue;
    }
    out.names.emplace_back(name);
  }
  return out;
}

// A path the way Windows' file functions will look it up. A "\\?\" or "\??\"
// path reaches the file system as written ("\\?\C:\a\user." names "user.",
// not "user"), so it is split as written. Any other path is first made
// absolute as those functions do it: std::filesystem::absolute calls
// GetFullPathNameW, the routine they use, which applies "." and "..", turns
// '/' into '\' and drops a trailing dot or space where Windows drops it. So
// the names walked are the names a later open of the same path looks up.
std::optional<location> locate(const fs::path &path) {
  const std::wstring &given = path.native();
  if (given.starts_with(L"\\\\?\\") || given.starts_with(L"\\??\\")) {
    return split(std::wstring_view(given).substr(4));
  }
  std::error_code ec;
  const fs::path absolute = fs::absolute(path, ec);
  if (ec) {
    return std::nullopt;
  }
  std::wstring text = absolute.native();
  std::ranges::replace(text, L'/', L'\\');
  // "\\.\C:\x" (and "//?/C:/x", which Windows normalizes like it) is C:\x.
  if (text.starts_with(L"\\\\?\\") || text.starts_with(L"\\\\.\\")) {
    text.erase(0, 4);
  }
  return split(text);
}

// What the walk passes to Windows for a path locate() made: "\\?\C:\...".
// The prefix lifts the MAX_PATH limit (the programs are not long-path aware,
// and a link can lead to a path longer than the one it sits in, which a
// later open follows all the same) and stops Windows from changing the
// names again.
std::wstring long_name(const fs::path &path) {
  return L"\\\\?\\" + path.native();
}

// "Volume{guid}\rest" (where a junction to a whole volume points): the
// volume's drive letter, from the mount manager, which opens nothing on the
// volume. A volume without a drive letter is not checked.
std::optional<location> from_volume(std::wstring_view name) {
  const std::size_t close = name.find(L'}');
  if (close == std::wstring_view::npos) {
    return std::nullopt;
  }
  const std::wstring volume =
      L"\\\\?\\" + std::wstring(name.substr(0, close + 1)) + L"\\";
  std::vector<wchar_t> mounts(4096, L'\0');
  DWORD length = 0;
  if (!GetVolumePathNamesForVolumeNameW(volume.c_str(), mounts.data(),
                                        DWORD(mounts.size()), &length)) {
    return std::nullopt;
  }
  for (const wchar_t *mount = mounts.data(); *mount != L'\0';
       mount += std::wcslen(mount) + 1) {
    const std::wstring_view text(mount);
    if (text.size() == 3 && is_drive(text)) {
      std::wstring_view rest = name.substr(close + 1);
      while (!rest.empty() && rest.front() == L'\\') {
        rest.remove_prefix(1);
      }
      return split(std::wstring(text) + std::wstring(rest));
    }
  }
  return std::nullopt;
}

// Where an NT name such as "\??\C:\x" (what links and subst drives store)
// is. Windows follows it as written. nullopt for names that are not a drive
// or a volume (a share among them, see split).
std::optional<location> from_nt(std::wstring_view name) {
  if (!name.starts_with(L"\\??\\")) {
    return std::nullopt;
  }
  name.remove_prefix(4);
  if (starts_with_nocase(name, L"Volume{")) {
    return from_volume(name);
  }
  return split(name);
}

struct drive {
  bool checked = false;           // a volume of this PC, walked by name
  bool is_link = false;           // a subst drive, a link at the root
  std::optional<location> target; // where a subst drive leads, if checkable
};

// What a drive letter stands for, from the name Windows keeps for it
// (QueryDosDeviceW), which opens nothing on the drive.
drive drive_of(const fs::path &root) {
  const std::wstring text = root.native(); // "C:\"
  const std::wstring letter = text.substr(0, 2);
  std::vector<wchar_t> device(32768, L'\0');
  if (QueryDosDeviceW(letter.c_str(), device.data(), DWORD(device.size())) ==
      0) {
    // No such drive: nothing on it exists, which the walk finds out.
    drive none;
    none.checked = GetLastError() == ERROR_FILE_NOT_FOUND &&
                   GetDriveTypeW(text.c_str()) == DRIVE_NO_ROOT_DIR;
    return none;
  }
  const std::wstring_view target(device.data());
  drive out;
  if (target.starts_with(L"\\??\\")) {
    out.is_link = true; // subst X: C:\folder
    out.target = from_nt(target);
    return out;
  }
  // A drive of this PC is a whole volume ("\Device\HarddiskVolume3"). A
  // network drive names a share after its redirector ("\Device\Lanman
  // Redirector\;Z:...\server\share"), and that share can be this PC's own
  // C:\ again (\\localhost\C$, see split); a folder after any other device is
  // just as unknown. Neither is checked.
  constexpr std::wstring_view devices = L"\\Device\\";
  const bool whole_device =
      target.starts_with(devices) && target.size() > devices.size() &&
      target.find(L'\\', devices.size()) == std::wstring_view::npos;
  const UINT type = GetDriveTypeW(text.c_str());
  out.checked = whole_device && type != DRIVE_REMOTE && type != DRIVE_UNKNOWN &&
                type != DRIVE_NO_ROOT_DIR;
  return out;
}

// The name a symbolic link or junction stores and Windows follows: its
// substitute name, not the print name shown to people (the two can differ).
struct link_text {
  std::wstring target;
  bool relative = false;
};

std::optional<link_text> read_link(const fs::path &link) {
  const HANDLE file = CreateFileW(
      long_name(link).c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }
  std::vector<unsigned char> data(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
  DWORD size = 0;
  const BOOL ok = DeviceIoControl(file, FSCTL_GET_REPARSE_POINT, nullptr, 0,
                                  data.data(), DWORD(data.size()), &size,
                                  nullptr);
  CloseHandle(file);
  if (!ok || size < 16) {
    return std::nullopt;
  }
  const auto u16 = [&](std::size_t at) {
    std::uint16_t value = 0;
    std::memcpy(&value, data.data() + at, sizeof(value));
    return std::size_t(value);
  };
  const auto u32 = [&](std::size_t at) {
    std::uint32_t value = 0;
    std::memcpy(&value, data.data() + at, sizeof(value));
    return value;
  };
  // REPARSE_DATA_BUFFER (ntifs.h, not in the SDK headers): tag, data length,
  // reserved, then the substitute name's offset and length and the print
  // name's offset and length. Symbolic links add a flags word. The names
  // follow, their offsets counted from there.
  std::size_t names = 16;
  link_text out;
  const std::uint32_t tag = u32(0);
  if (tag == IO_REPARSE_TAG_SYMLINK) {
    if (size < 20) {
      return std::nullopt;
    }
    names = 20;
    out.relative = (u32(16) & 1) != 0; // SYMLINK_FLAG_RELATIVE
  } else if (tag != IO_REPARSE_TAG_MOUNT_POINT) {
    return std::nullopt;
  }
  const std::size_t offset = u16(8);
  const std::size_t length = u16(10);
  if (length == 0 || length % 2 != 0 || names + offset + length > size) {
    return std::nullopt;
  }
  out.target.resize(length / 2);
  std::memcpy(out.target.data(), data.data() + names + offset, length);
  return out;
}

// Where the link at `link` (a location `root` is the root of) leads.
std::optional<location> link_destination(const fs::path &link,
                                         const fs::path &root,
                                         const link_text &text) {
  if (!text.relative) {
    return from_nt(text.target);
  }
  if (!text.target.empty() && text.target.front() == L'\\') {
    // "\x": from the root
    return split((root / text.target.substr(1)).native());
  }
  return split((link.parent_path() / text.target).native());
}

// Errors that mean the name is not there, so nothing below it exists either.
bool missing(DWORD error) {
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
         error == ERROR_DIRECTORY || error == ERROR_INVALID_NAME ||
         error == ERROR_BAD_NETPATH || error == ERROR_BAD_NET_NAME;
}

struct walk_result {
  boiii_identity_check where = boiii_identity_check::unknown;
  fs::path real; // with long names and every link replaced, when outside
};

// identity: the forms of the folder never to enter (empty while finding the
// identity folder's own real form).
walk_result walk(const fs::path &path, const std::vector<fs::path> &identity) {
  using enum boiii_identity_check;
  const auto hit = [&](const fs::path &candidate) {
    return std::ranges::any_of(
        identity, [&](const fs::path &form) { return within(candidate, form); });
  };
  std::optional<location> at = locate(path);
  if (!at) {
    return {};
  }
  int links = 0;
  for (;;) {
    if (hit(at->joined())) {
      return {inside, {}};
    }
    const drive root = drive_of(at->root);
    if (root.is_link) {
      if (!root.target || ++links > max_links) {
        return {};
      }
      location next = *root.target;
      next.names.insert(next.names.end(), at->names.begin(), at->names.end());
      at = std::move(next);
      continue;
    }
    if (!root.checked) {
      return {}; // a network drive, or a drive Windows does not describe
    }

    fs::path current = at->root;
    std::optional<location> relinked;
    for (std::size_t i = 0; i < at->names.size() && !relinked; ++i) {
      const std::wstring &name = at->names[i];
      if (!allowed_name(name)) {
        return {};
      }
      fs::path next = current / name;
      if (hit(next)) {
        return {inside, {}};
      }
      // The listing of `current` (already checked) answers for `name`; the
      // name itself is not opened.
      WIN32_FIND_DATAW found{};
      const HANDLE find =
          FindFirstFileExW(long_name(next).c_str(), FindExInfoBasic, &found,
                           FindExSearchNameMatch, nullptr, 0);
      if (find == INVALID_HANDLE_VALUE) {
        if (!missing(GetLastError())) {
          return {}; // a folder that cannot be listed, a name too long
        }
        // Nothing from here on exists, so none of it is a link.
        for (std::size_t j = i + 1; j < at->names.size(); ++j) {
          next /= at->names[j];
        }
        return {hit(next) ? inside : outside, next};
      }
      FindClose(find);
      current /= found.cFileName; // the long name, as stored
      if (hit(current)) {
        return {inside, {}};
      }
      // Other reparse points (OneDrive placeholders, deduplicated files) are
      // ordinary files and folders to Windows' name lookup; only name
      // surrogates send it elsewhere.
      if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 ||
          !IsReparseTagNameSurrogate(found.dwReserved0)) {
        continue;
      }
      if (++links > max_links) {
        return {};
      }
      const std::optional<link_text> text = read_link(current);
      if (!text) {
        return {}; // a kind of link this cannot read
      }
      std::optional<location> target =
          link_destination(current, at->root, *text);
      if (!target) {
        return {};
      }
      target->names.insert(target->names.end(), at->names.begin() + i + 1,
                           at->names.end());
      relinked = std::move(target);
    }
    if (!relinked) {
      return {hit(current) ? inside : outside, current};
    }
    at = std::move(relinked);
  }
}

// The identity folder as written, and as Windows really reaches it: the
// app data folder walked (it is the parent, so "user" itself is never looked
// up) with "user" added.
std::vector<fs::path> identity_forms(const fs::path &boiii_appdata) {
  const fs::path identity = boiii_appdata / "user";
  std::vector<fs::path> forms;
  const std::optional<location> written = locate(identity);
  forms.push_back(written ? written->joined() : identity.lexically_normal());
  const walk_result real = walk(boiii_appdata, {});
  if (real.where == boiii_identity_check::outside) {
    forms.push_back(real.real / "user");
  }
  return forms;
}
} // namespace

boiii_identity_check check_boiii_identity_path(const fs::path &path,
                                               const fs::path &boiii_appdata) {
  if (boiii_appdata.empty()) {
    return boiii_identity_check::outside;
  }
  return walk(path, identity_forms(boiii_appdata)).where;
}

bool is_boiii_identity_path(const fs::path &path, const fs::path &boiii_appdata) {
  return check_boiii_identity_path(path, boiii_appdata) !=
         boiii_identity_check::outside;
}
} // namespace xerces_core
