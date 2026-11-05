// Windows' own folder and file pickers (IFileDialog) for First run, Settings
// and Packs. They block the UI thread while open, as modal dialogs do.

#include "app.hpp"

#include <RmlUi_Include_Windows.h>

#include <objbase.h>
#include <shobjidl.h>

#include <vector>

#include "log.hpp"

namespace xerces_launcher {
namespace {
std::wstring widen(const std::string &text) {
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  std::wstring out(size > 0 ? std::size_t(size - 1) : 0, L'\0');
  if (size > 1) {
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
  }
  return out;
}

// "Name (*.ext)|*.ext|Other|*.*" to COMDLG_FILTERSPEC pairs.
std::vector<std::wstring> split_filter(const std::wstring &filter) {
  std::vector<std::wstring> parts;
  std::wstring current;
  for (const wchar_t c : filter) {
    if (c == L'|') {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  parts.push_back(current);
  return parts;
}

std::optional<fs::path> run_dialog(void *owner, REFCLSID kind,
                                   const std::string &title, DWORD extra,
                                   const std::wstring &filter,
                                   const std::wstring &default_name) {
  IFileDialog *dialog = nullptr;
  if (FAILED(CoCreateInstance(kind, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog)))) {
    log_error("launcher.picker.failed", "IFileDialog could not be created");
    return std::nullopt;
  }
  DWORD flags = 0;
  dialog->GetOptions(&flags);
  dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | extra);
  const std::wstring caption = widen(title);
  dialog->SetTitle(caption.c_str());
  const std::vector<std::wstring> parts = split_filter(filter);
  std::vector<COMDLG_FILTERSPEC> specs;
  for (std::size_t i = 0; i + 1 < parts.size(); i += 2) {
    specs.push_back({parts[i].c_str(), parts[i + 1].c_str()});
  }
  if (!specs.empty()) {
    dialog->SetFileTypes(UINT(specs.size()), specs.data());
    // The first filter's extension ("*.xpack.json" -> "xpack.json") is added
    // to a typed name that has none, so callers need not append it.
    const std::wstring &pattern = parts[1];
    if (pattern.size() > 2 && pattern.starts_with(L"*.") &&
        pattern.find(L'*', 1) == std::wstring::npos &&
        pattern.find(L';') == std::wstring::npos) {
      dialog->SetDefaultExtension(pattern.substr(2).c_str());
    }
  }
  if (!default_name.empty()) {
    dialog->SetFileName(default_name.c_str());
  }
  std::optional<fs::path> out;
  if (SUCCEEDED(dialog->Show(static_cast<HWND>(owner)))) {
    IShellItem *item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR name = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
        out = fs::path(name);
        CoTaskMemFree(name);
      }
      item->Release();
    }
  }
  dialog->Release();
  return out;
}
} // namespace

std::optional<fs::path> app::pick_folder(const std::string &title) {
  return run_dialog(window_.native_handle(), CLSID_FileOpenDialog, tr(title),
                    FOS_PICKFOLDERS | FOS_PATHMUSTEXIST, L"", L"");
}

std::optional<fs::path> app::pick_open_file(const std::string &title,
                                            const std::wstring &filter) {
  return run_dialog(window_.native_handle(), CLSID_FileOpenDialog, tr(title),
                    FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST, filter, L"");
}

std::optional<fs::path> app::pick_save_file(const std::string &title,
                                            const std::wstring &filter,
                                            const std::wstring &default_name) {
  return run_dialog(window_.native_handle(), CLSID_FileSaveDialog, tr(title),
                    FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST, filter,
                    default_name);
}
} // namespace xerces_launcher
